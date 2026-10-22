TARGET = nasdl
OBJS = main.o resume.o

INCDIR =
CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpsphttp -lpspnet_inet -lpsputility

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PS Linker NASDL

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
