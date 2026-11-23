TARGET = ALTUHAGAME
OBJS = main.o

CFLAGS = -O3 -ffast-math -fno-math-errno -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspgu -lpspdisplay -lpspctrl -lpsppower -lm

BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = altuha game
PSP_EBOOT_ICON = ICON0.PNG
PSP_EBOOT_PIC1 = PIC1.PNG
# animated XMB icon: used automatically as soon as ICON1.PMF is placed next to this Makefile
ifneq ($(wildcard ICON1.PMF),)
PSP_EBOOT_ICON1 = ICON1.PMF
endif

PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
