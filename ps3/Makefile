# PS Linker - PS3 (PSL1GHT) build
# Requires the PSL1GHT toolchain and ps3toolchain env vars loaded.

TARGET		:= nasdl
TITLE		:= PS Linker NASDL
APPID		:= PSNK00001

SRC		:= main.c resume.c
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
CFLAGS		:= -O2 -Wall -std=gnu99 -mcpu=cell -mminimal-toc $(MACHDEP) $(LIBPSL1GHT_INC)
LDFLAGS		:= $(MACHDEP) -mminimal-toc -Wl,-Map,$(TARGET).map
LIBPATHS	:= $(LIBPSL1GHT_LIB)
LIBS		:= -lnet -lsysmodule -lio -lm

all: $(TARGET).self

# Installable package (PSL1GHT's ppu_rules supplies the %.pkg recipe).
pkg: $(TARGET).pkg

run: $(TARGET).self
	ps3load $(TARGET).self

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).self $(TARGET).fake.self \
	      $(TARGET).pkg $(TARGET).gnpdrm.pkg $(TARGET).map
	rm -rf build

.PHONY: all clean pkg run
