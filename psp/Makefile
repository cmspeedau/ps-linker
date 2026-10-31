TARGET = nasdl
# ../ui objects build from the shared sources; each app keeps its own
# object files, so the two toolchains never overwrite each other's .o.
OBJS = main.o resume.o store.o ../ui/store_logic.o ../ui/catalog.o \
       ../ui/browser_model.o coverflow_psp.o gui.o store_gui.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -I../ui $(EXTRA_CFLAGS)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpsphttp -lpspnet_inet -lpsputility -lpspgu -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PS Linker NASDL

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
