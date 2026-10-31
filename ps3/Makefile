# PS Linker - PS3 (PSL1GHT) build
# Requires the PSL1GHT toolchain and ps3toolchain env vars loaded.

TARGET		:= nasdl
TITLE		:= PS Linker NASDL
APPID		:= PSNK00001

# Shared, toolchain-neutral sources compile straight from ../ui (the PSP
# app builds the same files with psp-gcc).
SRC		:= main.c resume.c http.c store.c \
		   ../ui/coverflow.c ../ui/catalog.c ../ui/store_logic.c

# The pixel renderer is the default UI; make TEXT_UI=1 restores the
# ANSI-TTY-only build (no RSX, no store pixel screens). Exactly one
# backend is compiled in.
ifneq ($(TEXT_UI),1)
SRC		+= ui_pix.c gfx.c store_gui.c
else
SRC		+= ui_text.c
endif

OBJS		:= $(SRC:.c=.o)
OFILES		:= $(OBJS)

# Locate the toolchain from the usual environment variables.  The
# compiler is invoked through the ppu- wrapper so this works with both
# the classic powerpc64-ps3-elf- prefix and newer ps3toolchain layouts.
PS3DEV		?= /usr/local/ps3dev
PSL1GHT		?= $(PS3DEV)

# make has a built-in LD=ld, so point it at the target compiler before
# PSL1GHT's rules are evaluated.
LD		:= $(PS3DEV)/ppu/bin/ppu-gcc
include $(PSL1GHT)/ppu_rules

# -mminimal-toc: keep the TOC small so r2 is set up consistently at
# C-startup.  Fixes the "access violation executing 0x7c0802a4" crash
# seen in RPCS3 (indirect call from CRT init with a garbage r2 / no
# .TOC. symbol in the linked ELF).
CFLAGS		:= -O2 -Wall -std=gnu99 -mcpu=cell -mminimal-toc $(MACHDEP) $(LIBPSL1GHT_INC) $(EXTRA_CFLAGS)
ifneq ($(TEXT_UI),1)
CFLAGS		+= -I$(PSL1GHT)/ppu/include
else
CFLAGS		+= -DNASDL_TEXT_UI
endif
LDFLAGS		:= $(MACHDEP) -mminimal-toc -Wl,-Map,$(TARGET).map
LIBPATHS	:= $(LIBPSL1GHT_LIB)
LIBS		:= -lnet -lsysmodule -lio -lm
ifneq ($(TEXT_UI),1)
LIBS		+= -lgcm_sys -lrsx -lsysutil
else
LIBS		+= -lsysutil
endif

all: $(TARGET).self

# Installable package (PSL1GHT's ppu_rules supplies the %.pkg recipe).
pkg: $(TARGET).pkg

# The ANSI-only variant, kept buildable so the TTY path cannot rot.
text-ui:
	$(MAKE) clean && $(MAKE) TEXT_UI=1

run: $(TARGET).self
	ps3load $(TARGET).self

clean:
	rm -f main.o resume.o http.o store.o ui_text.o ui_pix.o gfx.o \
	      store_gui.o ../ui/coverflow.o ../ui/catalog.o ../ui/store_logic.o \
	      $(TARGET).elf $(TARGET).self $(TARGET).fake.self \
	      $(TARGET).pkg $(TARGET).gnpdrm.pkg $(TARGET).map
	rm -rf build

.PHONY: all clean pkg run text-ui
