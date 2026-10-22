# PS Linker - PS3 (PSL1GHT) build
# Requires the PSL1GHT toolchain and ps3toolchain env vars loaded.

TARGET		:= nasdl
AUTHOR		:= cmspeedou

SRC		:= main.c
OBJS		:= $(SRC:.c=.o)

TITLE		:= PS Linker NASDL
APPID		:= PSNK00001

CFLAGS		:= -O2 -Wall -std=gnu99
LIBS		:= -lnet -lio -lm

BIN		:= $(TARGET).self
PKG		:= $(TARGET).pkg

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Build an installable package (requires pkg.py from psl1ght-utils)
pkg: $(BIN)
	pkg.py --contentid $(APPID)01-$(APPID) \
	       $(TARGET).$(APPID) $(PKG)

clean:
	rm -f $(OBJS) $(BIN) $(PKG)

run: $(BIN)
	ps3load $(BIN)

.PHONY: all clean pkg run
