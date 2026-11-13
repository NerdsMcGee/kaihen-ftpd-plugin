# kaihen-ftpd-plugin

An FTP server for the SD card on Switch 2, built as a
kaiHEN plugin. It starts at boot, before HEN, and
listens on port **5000**.

## Features

- Up to 4 clients at once, each on its own thread. A fifth client gets
  `421 Too many connections` instead of hanging.
- Passive mode only (`PASV` and `EPSV`).
- Upload, download, resume (`REST`) and append (`APPE`).
- Directory listing (`LIST`, including `LIST -la`, and `NLST`), `SIZE`,
  `MKD`, `RMD`, `DELE`, and rename (`RNFR`/`RNTO`).
- Transient `accept()` errors are retried, and the listening socket is
  rebuilt if it stops working, so the server does not exit.
- Idle clients are disconnected after 5 minutes. A data connection that is
  not opened within 30 seconds, or a transfer that stalls for 60 seconds, is
  dropped.

There is no authentication: any user name and password are accepted. Use it
only on a network you trust.

## Install

1. Create `/kaihen/plugins/` on the SD card if it does not exist.
2. Copy `ftpd.nso` into it.
3. Reboot. `/kaihen/plugin-status.txt` should list `ftpd: running`.
4. Connect an FTP client to the console's IP address, port 5000, in passive
   mode. The server answers `220 kaiHEN SD FTP ready`.

## Build

Requirements:

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev`.
- The ounce devkit (libounce and the kaiHEN plugin runtime) installed into
  `$DEVKITPRO`.

Then:

```sh
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
make
```

This produces `ftpd.nso`. Built and tested with ounce-devkit `20261113f`.

## Configuration

The settings are constants at the top of `source/main.c`:

| Constant | Default | Meaning |
|---|---|---|
| `FTP_PORT` | 5000 | Control port |
| `MAX_CLIENTS` | 4 | Clients served at once |
| `IDLE_TIMEOUT_MS` | 5 min | Idle control connection timeout |
| `DATA_ACCEPT_MS` | 30 s | Wait for the client's data connection |
| `DATA_IO_TIMEOUT_S` | 60 s | Stalled transfer timeout |

## License

ISC. See [LICENSE](LICENSE).
