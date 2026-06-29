# lumen — the AspisOS compositor / display server.
#
# Standalone component repo: it fetches a pinned glyph toolkit artifact (libs +
# headers) and builds against it, then packs a signed herald system package
# (lumen.hpkg) carrying /bin/lumen, its cap policy, and the desktop fonts/logo
# assets (so every component that depends on lumen gets fonts transitively).
#
#   make                fetch toolkit + build lumen.elf + pack lumen.hpkg
#   HERALD_KEY=<key>    signing key for the package (required for `make`/pack)
#   MUSL_CC=<musl-gcc>  musl cross-compiler (defaults to PATH musl-gcc)
MUSL_CC ?= musl-gcc
VERSION       := $(shell cat VERSION)
GLYPH_VERSION := $(shell cat GLYPH_VERSION)

CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall \
         -DAEGIS_VERSION=\"$(VERSION)\" -Itoolkit/include
SRCS = $(addprefix src/, main.c cursor.c compositor.c terminal.c about.c lumen_server.c)

all: lumen.hpkg

# Pinned toolkit artifact (the kernel-style fetched dependency).
toolkit/include/glyph.h:
	sh tools/fetch-glyph.sh $(GLYPH_VERSION) toolkit

lumen.elf: $(SRCS) toolkit/include/glyph.h
	$(MUSL_CC) $(CFLAGS) -o $@ $(SRCS) -Ltoolkit/lib -lcitadel -lglyph

lumen.hpkg: lumen.elf
	sh tools/pack.sh

clean:
	rm -f lumen.elf lumen.hpkg lumen.hpkg.sig
	rm -rf toolkit
