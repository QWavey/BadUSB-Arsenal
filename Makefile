# GhostLink firmware — Makefile
#
# Target: RV1106 (Cortex-A7, armv7-a, hard float). Default compiler is the
# Luckfox buildroot toolchain. Override with CROSS_COMPILE=... from the env.
#
#   make                        - cross build into out/ghostlink
#   make host                   - native build for local smoke testing
#   make install SYSROOT=...    - stage overlay files into a sysroot
#   make pack                   - produce an SD overlay zip
#   make clean

CROSS_COMPILE ?= arm-rockchip830-linux-uclibcgnueabihf-
CC            := $(CROSS_COMPILE)gcc
STRIP         := $(CROSS_COMPILE)strip

ifeq ($(MAKECMDGOALS),host)
CC    := gcc
STRIP := strip
endif

OUT       := out
BIN       := $(OUT)/ghostlink

CFLAGS    := -Os -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
             -ffunction-sections -fdata-sections \
             -D_GNU_SOURCE -std=gnu11 -pthread -MMD -MP
LDFLAGS   := -Wl,--gc-sections -pthread

SRC := \
  src/common/log.c \
  src/core/config.c \
  src/core/supervisor.c \
  src/core/main.c \
  src/net/gadget.c \
  src/net/bridge.c \
  src/net/deadnet.c \
  src/net/redirect.c \
  src/record/recorder.c \
  src/hid/layouts.c \
  src/hid/ducky.c \
  src/web/httpd.c \
  src/web/pages_embed.c

OBJ := $(SRC:%.c=$(OUT)/%.o)

# Header dependencies written by -MMD -MP, so editing config.h rebuilds users.
-include $(OBJ:.o=.d)

all: $(BIN)

host: all

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^
	$(STRIP) $@
	@ls -la $@

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	@test -n "$(SYSROOT)" || (echo "set SYSROOT=/path/to/rootfs"; exit 1)
	mkdir -p $(SYSROOT)/usr/sbin
	cp $(BIN) $(SYSROOT)/usr/sbin/ghostlink
	mkdir -p $(SYSROOT)/etc/init.d
	cp overlay/etc/init.d/S99ghost $(SYSROOT)/etc/init.d/
	chmod +x $(SYSROOT)/etc/init.d/S99ghost

pack: all
	mkdir -p dist
	cp -r sdcard-template dist/sd
	cp $(BIN) dist/sd/ghostlink
	cd dist && zip -r ghostlink-sd.zip sd && echo "built dist/ghostlink-sd.zip"

clean:
	rm -rf $(OUT) dist

.PHONY: all host install pack clean
