TARGET = nasdl
OBJS = main.o resume.o store.o store_logic.o coverflow_psp.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -I../ui
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpsphttp -lpspnet_inet -lpsputility

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PS Linker NASDL

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
