// SPDX-License-Identifier: ISC
//
// ftpd: an FTP server for the SD card, built as a kaiHEN plugin
// (/kaihen/plugins/ftpd.nso). Listens on port 5000.
//
// Replaces the original ftpd-5000.nso, which:
//   - ran one session at a time on the main thread, so a second client got
//     no greeting until the first one quit;
//   - started BSD with sb_efficiency = 1, so the socket buffer pool fit only
//     one connection;
//   - shut the whole server down on the first accept() error.
//
// This version serves up to MAX_CLIENTS sessions, each on its own thread,
// turns away extra clients with "421", retries transient accept() errors,
// rebuilds the listening socket if it stops working, and times out idle or
// stalled clients.

#include <switch.h>
#include <kaihen/plugin.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define FTP_PORT            5000
#define MAX_CLIENTS         4
#define LISTEN_BACKLOG      8
#define XFER_BUF_SIZE       0x8000
#define CMD_MAX             1024
#define DIR_BATCH           8
#define SESSION_STACK_SIZE  0x8000
#define SESSION_PRIORITY    0x2C
#define IDLE_TIMEOUT_MS     (5 * 60 * 1000)
#define DATA_ACCEPT_MS      30000
#define DATA_IO_TIMEOUT_S   60
#define ONE_SECOND_NS       1000000000ULL

// Plugin process setup (see <kaihen/plugin.h>).
u32    __nx_applet_type     = AppletType_None;
size_t __nx_heap_size       = 0x200000;
u32    __nx_fs_num_sessions = 3;
void __appInit(void) {}
void __appExit(void) {}

static FsFileSystem g_sd;

enum { SLOT_FREE = 0, SLOT_RUNNING = 1, SLOT_DONE = 2 };

typedef struct {
    Thread thread;
    int    state;
    int    fd;
} Slot;

static Slot g_slots[MAX_CLIENTS];

typedef struct {
    int    ctrl;
    int    pasv;
    int    data;
    s64    rest;
    bool   have_rnfr;
    size_t rpos, rlen;
    char   cwd[FS_MAX_PATH];
    char   rnfr[FS_MAX_PATH];
    char   path[FS_MAX_PATH];
    char   path2[FS_MAX_PATH];
    char   line[CMD_MAX];
    char   rbuf[1024];
    char   reply_buf[FS_MAX_PATH + 128];
    FsDirectoryEntry dirents[DIR_BATCH];
    u8     xfer[XFER_BUF_SIZE];
} Session;

typedef enum { DATA_OK, DATA_FAILED, CTRL_LOST } DataStatus;

//-----------------------------------------------------------------------------
// Sockets
//-----------------------------------------------------------------------------

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static bool send_all(int fd, const void *buf, size_t len) {
    const u8 *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// Sends "<code> <text>\r\n" on the control connection.
static bool reply(Session *s, int code, const char *fmt, ...) {
    char *buf = s->reply_buf;
    size_t cap = sizeof s->reply_buf - 2;
    int n = snprintf(buf, cap, "%d ", code);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
    if (len > cap - 1)
        len = cap - 1;
    buf[len++] = '\r';
    buf[len++] = '\n';
    return send_all(s->ctrl, buf, len);
}

// Reads one command line. Returns 1 for a line, 0 if the client went away,
// -1 on idle timeout. Overlong lines are cut at CMD_MAX - 1 bytes.
static int read_line(Session *s) {
    size_t len = 0;
    for (;;) {
        if (s->rpos == s->rlen) {
            struct pollfd p = { .fd = s->ctrl, .events = POLLIN };
            int r = poll(&p, 1, IDLE_TIMEOUT_MS);
            if (r == 0)
                return -1;
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                return 0;
            }
            ssize_t n = recv(s->ctrl, s->rbuf, sizeof s->rbuf, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return 0;
            s->rpos = 0;
            s->rlen = (size_t)n;
        }
        char c = s->rbuf[s->rpos++];
        if (c == '\n') {
            if (len && s->line[len - 1] == '\r')
                len--;
            s->line[len] = '\0';
            return 1;
        }
        if (len < CMD_MAX - 1)
            s->line[len++] = c;
    }
}

//-----------------------------------------------------------------------------
// Paths and files
//-----------------------------------------------------------------------------

// Joins arg to cwd and removes ".", ".." and repeated slashes. The result
// always starts with '/'. Returns false if it does not fit in FS_MAX_PATH.
static bool resolve_path(const char *cwd, const char *arg, char *out) {
    char tmp[FS_MAX_PATH * 2];
    int n;
    if (!arg || !*arg)
        n = snprintf(tmp, sizeof tmp, "%s", cwd);
    else if (arg[0] == '/')
        n = snprintf(tmp, sizeof tmp, "%s", arg);
    else
        n = snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return false;

    size_t olen = 1;
    out[0] = '/';
    out[1] = '\0';
    const char *p = tmp;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t sl = (size_t)(p - seg);
        if (sl == 1 && seg[0] == '.')
            continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            while (olen > 1 && out[olen - 1] != '/')
                olen--;
            if (olen > 1)
                olen--;
            out[olen] = '\0';
            continue;
        }
        size_t need = (olen > 1 ? 1 : 0) + sl;
        if (olen + need >= FS_MAX_PATH)
            return false;
        if (olen > 1)
            out[olen++] = '/';
        memcpy(out + olen, seg, sl);
        olen += sl;
        out[olen] = '\0';
    }
    return true;
}

static bool entry_type(const char *path, FsDirEntryType *type) {
    return R_SUCCEEDED(fsFsGetEntryType(&g_sd, path, type));
}

static bool file_size(const char *path, s64 *size) {
    FsFile f;
    if (R_FAILED(fsFsOpenFile(&g_sd, path, FsOpenMode_Read, &f)))
        return false;
    Result rc = fsFileGetSize(&f, size);
    fsFileClose(&f);
    return R_SUCCEEDED(rc);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

//-----------------------------------------------------------------------------
// Data connection (passive mode only)
//-----------------------------------------------------------------------------

static bool open_pasv(Session *s, struct sockaddr_in *bound) {
    close_fd(&s->pasv);
    s->pasv = socket(AF_INET, SOCK_STREAM, 0);
    if (s->pasv < 0)
        return false;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    socklen_t len = sizeof *bound;
    if (bind(s->pasv, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(s->pasv, 1) < 0 ||
        getsockname(s->pasv, (struct sockaddr *)bound, &len) < 0) {
        close_fd(&s->pasv);
        return false;
    }
    return true;
}

// Waits for the client's data connection, then closes the passive listener.
static bool accept_data(Session *s) {
    struct pollfd p = { .fd = s->pasv, .events = POLLIN };
    int r;
    do {
        r = poll(&p, 1, DATA_ACCEPT_MS);
    } while (r < 0 && errno == EINTR);
    if (r > 0 && (p.revents & POLLIN))
        s->data = accept(s->pasv, NULL, NULL);
    close_fd(&s->pasv);
    if (s->data < 0)
        return false;
    struct timeval tv = { .tv_sec = DATA_IO_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(s->data, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s->data, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return true;
}

static DataStatus begin_data(Session *s) {
    if (s->pasv < 0)
        return reply(s, 425, "Use PASV or EPSV first") ? DATA_FAILED : CTRL_LOST;
    if (!reply(s, 150, "Opening data connection"))
        return CTRL_LOST;
    if (!accept_data(s))
        return reply(s, 425, "Cannot open data connection") ? DATA_FAILED : CTRL_LOST;
    return DATA_OK;
}

static bool end_data(Session *s, bool ok) {
    close_fd(&s->data);
    return ok ? reply(s, 226, "Transfer complete") : reply(s, 426, "Transfer failed");
}

//-----------------------------------------------------------------------------
// Commands. Each returns false only when the session must end.
//-----------------------------------------------------------------------------

static bool cmd_pasv(Session *s, bool extended) {
    struct sockaddr_in bound;
    if (!open_pasv(s, &bound))
        return reply(s, 425, "Cannot open data socket");
    unsigned port = ntohs(bound.sin_port);
    if (extended)
        return reply(s, 229, "Entering Extended Passive Mode (|||%u|)", port);

    struct sockaddr_in local;
    socklen_t len = sizeof local;
    if (getsockname(s->ctrl, (struct sockaddr *)&local, &len) < 0) {
        close_fd(&s->pasv);
        return reply(s, 425, "Cannot locate control address");
    }
    u32 ip = ntohl(local.sin_addr.s_addr);
    return reply(s, 227, "Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                 (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255,
                 (unsigned)(ip >> 8) & 255, (unsigned)ip & 255,
                 port >> 8, port & 255);
}

static bool list_entry(Session *s, size_t *used, const FsDirectoryEntry *e, bool names_only) {
    if (*used + 0x400 > sizeof s->xfer) {
        if (!send_all(s->data, s->xfer, *used))
            return false;
        *used = 0;
    }
    char *out = (char *)s->xfer + *used;
    size_t cap = sizeof s->xfer - *used;
    int n;
    if (names_only)
        n = snprintf(out, cap, "%s\r\n", e->name);
    else if (e->type == FsDirEntryType_Dir)
        n = snprintf(out, cap, "drwxr-xr-x 1 owner group 0 Jan  1 00:00 %s\r\n", e->name);
    else
        n = snprintf(out, cap, "-rw-r--r-- 1 owner group %lld Jan  1 00:00 %s\r\n",
                     (long long)e->file_size, e->name);
    if (n > 0)
        *used += (size_t)n < cap ? (size_t)n : cap - 1;
    return true;
}

static bool cmd_list(Session *s, const char *arg, bool names_only) {
    // Skip ls-style options such as "-la".
    while (*arg == '-') {
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }
    if (!resolve_path(s->cwd, arg, s->path))
        return reply(s, 550, "Invalid path");
    FsDirEntryType type;
    if (!entry_type(s->path, &type))
        return reply(s, 550, "No such file or directory");

    bool is_dir = type == FsDirEntryType_Dir;
    FsDir dir;
    if (is_dir) {
        if (R_FAILED(fsFsOpenDirectory(&g_sd, s->path,
                                       FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir)))
            return reply(s, 550, "Directory unavailable");
    } else {
        FsDirectoryEntry *e = &s->dirents[0];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", base_name(s->path));
        e->type = FsDirEntryType_File;
        if (!file_size(s->path, &e->file_size))
            return reply(s, 550, "File unavailable");
    }

    DataStatus ds = begin_data(s);
    if (ds != DATA_OK) {
        if (is_dir)
            fsDirClose(&dir);
        return ds == DATA_FAILED;
    }

    bool ok = true;
    size_t used = 0;
    if (is_dir) {
        for (;;) {
            s64 count = 0;
            if (R_FAILED(fsDirRead(&dir, &count, DIR_BATCH, s->dirents))) {
                ok = false;
                break;
            }
            if (count <= 0)
                break;
            for (s64 i = 0; i < count && ok; i++)
                ok = list_entry(s, &used, &s->dirents[i], names_only);
            if (!ok)
                break;
        }
        fsDirClose(&dir);
    } else {
        ok = list_entry(s, &used, &s->dirents[0], names_only);
    }
    if (ok && used)
        ok = send_all(s->data, s->xfer, used);
    return end_data(s, ok);
}

static bool cmd_retr(Session *s, const char *arg) {
    s64 rest = s->rest;
    s->rest = 0;
    if (!resolve_path(s->cwd, arg, s->path))
        return reply(s, 550, "Invalid path");
    FsFile f;
    if (R_FAILED(fsFsOpenFile(&g_sd, s->path, FsOpenMode_Read, &f)))
        return reply(s, 550, "File unavailable");
    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&f, &size)) || rest > size) {
        fsFileClose(&f);
        return reply(s, 550, "File unavailable");
    }

    DataStatus ds = begin_data(s);
    if (ds != DATA_OK) {
        fsFileClose(&f);
        return ds == DATA_FAILED;
    }

    bool ok = true;
    s64 off = rest;
    while (off < size) {
        u64 want = (u64)(size - off);
        if (want > sizeof s->xfer)
            want = sizeof s->xfer;
        u64 got = 0;
        if (R_FAILED(fsFileRead(&f, off, s->xfer, want, FsReadOption_None, &got)) || got == 0 ||
            !send_all(s->data, s->xfer, got)) {
            ok = false;
            break;
        }
        off += (s64)got;
    }
    fsFileClose(&f);
    return end_data(s, ok);
}

static bool cmd_store(Session *s, const char *arg, bool append) {
    s64 rest = s->rest;
    s->rest = 0;
    if (!resolve_path(s->cwd, arg, s->path))
        return reply(s, 550, "Invalid path");

    FsDirEntryType type;
    bool exists = entry_type(s->path, &type);
    if (exists && type != FsDirEntryType_File)
        return reply(s, 550, "Not a file");
    if (exists && !append && rest == 0) {
        if (R_FAILED(fsFsDeleteFile(&g_sd, s->path)))
            return reply(s, 550, "Cannot replace file");
        exists = false;
    }
    if (!exists && R_FAILED(fsFsCreateFile(&g_sd, s->path, 0, 0)))
        return reply(s, 550, "Cannot create file");

    FsFile f;
    if (R_FAILED(fsFsOpenFile(&g_sd, s->path, FsOpenMode_Write | FsOpenMode_Append, &f)))
        return reply(s, 550, "Cannot open file");

    s64 off = 0;
    if (append || rest > 0) {
        s64 size = 0;
        bool good = R_SUCCEEDED(fsFileGetSize(&f, &size));
        if (good && !append)
            good = rest <= size && R_SUCCEEDED(fsFileSetSize(&f, rest));
        if (!good) {
            fsFileClose(&f);
            return reply(s, 550, "Cannot resume file");
        }
        off = append ? size : rest;
    }

    DataStatus ds = begin_data(s);
    if (ds != DATA_OK) {
        fsFileClose(&f);
        return ds == DATA_FAILED;
    }

    bool ok = true;
    for (;;) {
        ssize_t n = recv(s->data, s->xfer, sizeof s->xfer, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        if (R_FAILED(fsFileWrite(&f, off, s->xfer, (u64)n, FsWriteOption_None))) {
            ok = false;
            break;
        }
        off += n;
    }
    if (R_FAILED(fsFileFlush(&f)))
        ok = false;
    fsFileClose(&f);
    fsFsCommit(&g_sd);
    return end_data(s, ok);
}

static bool cmd_cwd(Session *s, const char *arg) {
    FsDirEntryType type;
    if (!resolve_path(s->cwd, arg, s->path))
        return reply(s, 550, "Invalid path");
    if (!entry_type(s->path, &type) || type != FsDirEntryType_Dir)
        return reply(s, 550, "Directory unavailable");
    memcpy(s->cwd, s->path, sizeof s->cwd);
    return reply(s, 250, "Directory changed");
}

static bool cmd_rnto(Session *s, const char *arg) {
    if (!s->have_rnfr)
        return reply(s, 503, "Use RNFR first");
    s->have_rnfr = false;
    FsDirEntryType type;
    if (!resolve_path(s->cwd, arg, s->path2))
        return reply(s, 550, "Invalid path");
    if (!entry_type(s->rnfr, &type))
        return reply(s, 550, "No such file or directory");
    Result rc = type == FsDirEntryType_Dir ? fsFsRenameDirectory(&g_sd, s->rnfr, s->path2)
                                           : fsFsRenameFile(&g_sd, s->rnfr, s->path2);
    fsFsCommit(&g_sd);
    return R_SUCCEEDED(rc) ? reply(s, 250, "Renamed") : reply(s, 550, "Cannot rename");
}

static bool handle(Session *s, char *line) {
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ')
            arg++;
    } else {
        arg = line + strlen(line);
    }
    for (char *p = line; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';

#define IS(name) (strcmp(line, name) == 0)
    if (!IS("RNTO"))
        s->have_rnfr = false;

    if (IS("USER") || IS("PASS"))
        return reply(s, 230, "Logged in");
    if (IS("SYST"))
        return reply(s, 215, "UNIX Type: L8");
    if (IS("TYPE") || IS("OPTS") || IS("NOOP") || IS("MODE") || IS("STRU"))
        return reply(s, 200, "OK");
    if (IS("FEAT")) {
        static const char feat[] =
            "211-Features\r\n EPSV\r\n PASV\r\n SIZE\r\n REST STREAM\r\n UTF8\r\n211 End\r\n";
        return send_all(s->ctrl, feat, sizeof feat - 1);
    }
    if (IS("QUIT")) {
        reply(s, 221, "Bye");
        return false;
    }
    if (IS("PWD") || IS("XPWD"))
        return reply(s, 257, "\"%s\" is the current directory", s->cwd);
    if (IS("CWD") || IS("XCWD"))
        return cmd_cwd(s, arg);
    if (IS("CDUP") || IS("XCUP"))
        return cmd_cwd(s, "..");
    if (IS("PASV"))
        return cmd_pasv(s, false);
    if (IS("EPSV"))
        return cmd_pasv(s, true);
    if (IS("LIST"))
        return cmd_list(s, arg, false);
    if (IS("NLST"))
        return cmd_list(s, arg, true);
    if (IS("RETR"))
        return cmd_retr(s, arg);
    if (IS("STOR"))
        return cmd_store(s, arg, false);
    if (IS("APPE"))
        return cmd_store(s, arg, true);
    if (IS("REST")) {
        char *end = NULL;
        long long v = strtoll(arg, &end, 10);
        if (!*arg || (end && *end) || v < 0)
            return reply(s, 501, "Invalid offset");
        s->rest = v;
        return reply(s, 350, "Restarting at %lld", v);
    }
    if (IS("SIZE")) {
        s64 size;
        FsDirEntryType type;
        if (!resolve_path(s->cwd, arg, s->path) || !entry_type(s->path, &type) ||
            type != FsDirEntryType_File || !file_size(s->path, &size))
            return reply(s, 550, "File unavailable");
        return reply(s, 213, "%lld", (long long)size);
    }
    if (IS("DELE")) {
        if (!resolve_path(s->cwd, arg, s->path) || R_FAILED(fsFsDeleteFile(&g_sd, s->path)))
            return reply(s, 550, "Cannot delete file");
        fsFsCommit(&g_sd);
        return reply(s, 250, "OK");
    }
    if (IS("MKD") || IS("XMKD")) {
        if (!resolve_path(s->cwd, arg, s->path) || R_FAILED(fsFsCreateDirectory(&g_sd, s->path)))
            return reply(s, 550, "Cannot create directory");
        fsFsCommit(&g_sd);
        return reply(s, 257, "\"%s\" created", s->path);
    }
    if (IS("RMD") || IS("XRMD")) {
        if (!resolve_path(s->cwd, arg, s->path) || R_FAILED(fsFsDeleteDirectory(&g_sd, s->path)))
            return reply(s, 550, "Cannot remove directory");
        fsFsCommit(&g_sd);
        return reply(s, 250, "OK");
    }
    if (IS("RNFR")) {
        FsDirEntryType type;
        if (!resolve_path(s->cwd, arg, s->rnfr) || !entry_type(s->rnfr, &type))
            return reply(s, 550, "No such file or directory");
        s->have_rnfr = true;
        return reply(s, 350, "Ready for RNTO");
    }
    if (IS("RNTO"))
        return cmd_rnto(s, arg);
#undef IS
    return reply(s, 502, "Command not implemented");
}

//-----------------------------------------------------------------------------
// Sessions and the accept loop
//-----------------------------------------------------------------------------

static void session_main(void *arg) {
    Slot *slot = arg;
    Session *s = calloc(1, sizeof *s);
    if (s) {
        s->ctrl = slot->fd;
        s->pasv = -1;
        s->data = -1;
        strcpy(s->cwd, "/");
        if (reply(s, 220, "kaiHEN SD FTP ready")) {
            for (;;) {
                int r = read_line(s);
                if (r < 0) {
                    reply(s, 421, "Idle timeout");
                    break;
                }
                if (r == 0 || !handle(s, s->line))
                    break;
            }
        }
        close_fd(&s->data);
        close_fd(&s->pasv);
        free(s);
    }
    close_fd(&slot->fd);
    __atomic_store_n(&slot->state, SLOT_DONE, __ATOMIC_RELEASE);
}

static void reap_slots(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Slot *slot = &g_slots[i];
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == SLOT_DONE) {
            threadWaitForExit(&slot->thread);
            threadClose(&slot->thread);
            slot->state = SLOT_FREE;
        }
    }
}

static void reject(int fd) {
    static const char msg[] = "421 Too many connections, try again later\r\n";
    send(fd, msg, sizeof msg - 1, 0);
    close(fd);
}

static void start_session(int fd) {
    reap_slots();
    Slot *slot = NULL;
    for (int i = 0; i < MAX_CLIENTS && !slot; i++)
        if (g_slots[i].state == SLOT_FREE)
            slot = &g_slots[i];
    if (!slot) {
        reject(fd);
        return;
    }
    slot->fd = fd;
    if (R_FAILED(threadCreate(&slot->thread, session_main, slot, NULL,
                              SESSION_STACK_SIZE, SESSION_PRIORITY, -2))) {
        slot->fd = -1;
        reject(fd);
        return;
    }
    slot->state = SLOT_RUNNING;
    if (R_FAILED(threadStart(&slot->thread))) {
        threadClose(&slot->thread);
        slot->state = SLOT_FREE;
        slot->fd = -1;
        reject(fd);
    }
}

static int make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(FTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, LISTEN_BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Waits up to a second for a client. Returns false if the listener is broken
// and must be rebuilt.
static bool accept_one(int listener, int *failures) {
    struct pollfd p = { .fd = listener, .events = POLLIN };
    int r = poll(&p, 1, 1000);
    if (r == 0 || (r < 0 && errno == EINTR))
        return true;
    if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return false;

    int fd = accept(listener, NULL, NULL);
    if (fd < 0) {
        if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        // ENOBUFS, EMFILE and the like: back off, rebuild after repeated failures.
        svcSleepThread(ONE_SECOND_NS / 10);
        return ++*failures < 5;
    }
    *failures = 0;
    start_session(fd);
    return true;
}

static void serve(Handle ready) {
    bool signaled = false;
    int listener = -1;
    int failures = 0;
    for (;;) {
        reap_slots();
        if (listener < 0) {
            listener = make_listener();
            if (!signaled) {
                // Do not hold up the bootstrap if the network is not up yet;
                // the listener is retried below.
                svcSignalEvent(ready);
                signaled = true;
            }
            if (listener < 0) {
                svcSleepThread(ONE_SECOND_NS);
                continue;
            }
            failures = 0;
        }
        if (!accept_one(listener, &failures)) {
            close_fd(&listener);
            svcSleepThread(ONE_SECOND_NS);
        }
    }
}

static Result sockets_init(void) {
    SocketInitConfig cfg = {
        // 64 KiB per direction, fixed, with 4x the buffer pool of the
        // original (which used sb_efficiency = 1): 512 KiB of transfer memory.
        .tcp_tx_buf_size     = 0x10000,
        .tcp_rx_buf_size     = 0x10000,
        .tcp_tx_buf_max_size = 0,
        .tcp_rx_buf_max_size = 0,
        .udp_tx_buf_size     = 0,
        .udp_rx_buf_size     = 0,
        .sb_efficiency       = 4,
        // One BSD session per thread that can block in a socket call.
        .num_bsd_sessions    = MAX_CLIENTS + 1,
        .bsd_service_type    = BsdServiceType_Auto,
    };
    Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        // Fewer sessions still work; threads then take turns.
        cfg.num_bsd_sessions = 2;
        rc = socketInitialize(&cfg);
    }
    return rc;
}

int main(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        g_slots[i].fd = -1;

    Handle ready;
    Result rc = pluginRuntimeInitialize(&ready);
    if (R_FAILED(rc))
        return rc;

    rc = fsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = fsOpenSdCardFileSystem(&g_sd);
        if (R_SUCCEEDED(rc)) {
            rc = sockets_init();
            if (R_SUCCEEDED(rc)) {
                serve(ready); // does not return
                socketExit();
            }
            fsFsClose(&g_sd);
        }
        fsExit();
    }
    pluginRuntimeExit();
    return rc;
}
