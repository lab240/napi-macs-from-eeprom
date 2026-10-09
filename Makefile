# Makefile - napi-set-mac (NAPI EEPROM MAC assignment), C version
#
#   make                         build build/napi-set-mac with $(CC)
#   make test                    build and run unit tests with $(HOSTCC)
#   make install DESTDIR=...     install binary, udev rule, systemd unit, config
#
# Cross build for a board without a matching sysroot (static, any glibc):
#   make CC=aarch64-linux-gnu-gcc LDFLAGS=-static
#
# Yocto passes CC/CFLAGS/LDFLAGS and the directory variables below
# (see yocto/napi-mac_git.bb).

VERSION		?= 13.0

CC		?= cc
HOSTCC		?= cc
CFLAGS		?= -O2 -g
LDFLAGS		?=
WARN		:= -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
CPPFLAGS	+= -DNAPI_VERSION=\"$(VERSION)\"

UDEVDIR		?= /usr/lib/udev
UDEVRULESDIR	?= $(UDEVDIR)/rules.d
SYSTEMDUNITDIR	?= /usr/lib/systemd/system
SYSCONFDIR	?= /etc

SRCS		:= src/config.c src/eeprom.c src/classify.c src/netif.c src/napi_mac.c
HDRS		:= src/napi_mac.h

# Installed files carry the helper path; rewrite it when UDEVDIR differs.
FIXPATH		:= sed 's|/usr/lib/udev/napi-set-mac|$(UDEVDIR)/napi-set-mac|g'

all: build/napi-set-mac

build:
	mkdir -p build

build/napi-set-mac: $(SRCS) src/main.c $(HDRS) | build
	$(CC) $(WARN) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) src/main.c $(LDFLAGS)

build/test-napi-mac: $(SRCS) tests/test_napi_mac.c $(HDRS) | build
	$(HOSTCC) $(WARN) -Wno-format-truncation $(CPPFLAGS) -O1 -g -Isrc -o $@ $(SRCS) tests/test_napi_mac.c

# Same tests built for the target, to run on the board.
build/test-napi-mac.target: $(SRCS) tests/test_napi_mac.c $(HDRS) | build
	$(CC) $(WARN) -Wno-format-truncation $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ $(SRCS) tests/test_napi_mac.c $(LDFLAGS)

test: build/test-napi-mac
	./build/test-napi-mac

install: build/napi-set-mac
	install -D -m 755 build/napi-set-mac $(DESTDIR)$(UDEVDIR)/napi-set-mac
	install -d $(DESTDIR)$(UDEVRULESDIR) $(DESTDIR)$(SYSTEMDUNITDIR)
	$(FIXPATH) etc/udev/rules.d/75-napi-mac.rules > $(DESTDIR)$(UDEVRULESDIR)/75-napi-mac.rules
	$(FIXPATH) usr/lib/systemd/system/napi-mac.service > $(DESTDIR)$(SYSTEMDUNITDIR)/napi-mac.service
	chmod 644 $(DESTDIR)$(UDEVRULESDIR)/75-napi-mac.rules $(DESTDIR)$(SYSTEMDUNITDIR)/napi-mac.service
	install -D -m 644 etc/napi/mac.conf $(DESTDIR)$(SYSCONFDIR)/napi/mac.conf

clean:
	rm -rf build

.PHONY: all test install clean
