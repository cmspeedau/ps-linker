# PS Linker - PS3 (PSL1GHT) build

TARGET		:= nasdl
TITLE		:= PS Linker NASDL
APPID		:= PSNK00001

SRC		:= main.c resume.c
OBJS		:= $(SRC:.c=.o)
OFILES		:= $(OBJS)


PS3DEV		?= /usr/local/ps3dev
PSL1GHT		?= $(PS3DEV)

# PSL1GHT's rules are evaluated.
LD		:= $(PS3DEV)/ppu/bin/ppu-gcc
include $(PSL1GHT)/ppu_rules


# .TOC. symbol in the linked ELF).
LIBPATHS	:= $(LIBPSL1GHT_LIB)
LIBS		:= -lnet -lsysmodule -lio -lm

all: $(TARGET).self

pkg: $(TARGET).pkg

run: $(TARGET).self
	ps3load $(TARGET).self

clean:
	      $(TARGET).pkg $(TARGET).gnpdrm.pkg $(TARGET).map
	rm -rf build

.PHONY: all clean pkg run
