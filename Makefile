#---------------------------------------------------------------------------------
# ftpd: kaiHEN plugin (NSO) for Switch 2, built with libounce.
#
#   make            builds ftpd.nso
#   make clean
#
# Install: copy ftpd.nso to /kaihen/plugins/ on the SD card.
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

include $(DEVKITPRO)/libounce/switch2_rules

TARGET   := ftpd
BUILD    := build
SOURCES  := source

ARCH     := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS   := -g -Wall -Wextra -Wno-unused-parameter -O2 -ffunction-sections -fdata-sections \
            $(ARCH) $(CPPFLAGS) -I$(LIBNX)/include
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map
# libounce without mimalloc (see <kaihen/plugin.h>).
LIBS     := -lkaihen_plugin -lounce_core
LIBDIRS  := -L$(PORTLIBS_SWITCH2)/lib -L$(LIBNX)/lib

CFILES   := $(wildcard $(SOURCES)/*.c)
OFILES   := $(patsubst $(SOURCES)/%.c,$(BUILD)/%.o,$(CFILES))

.PHONY: all clean
all: $(TARGET).nso

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: $(SOURCES)/%.c | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OFILES)
	@echo linking $(notdir $@)
	@$(CC) $(LDFLAGS) $(OFILES) $(LIBDIRS) $(LIBS) -o $@

$(TARGET).nso: $(BUILD)/$(TARGET).elf
	@elf2nso $< $@
	@echo built ... $(notdir $@)

clean:
	@rm -rf $(BUILD) $(TARGET).nso

-include $(OFILES:.o=.d)
