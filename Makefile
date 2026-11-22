TARGET = DUSKCRUSHER
OBJS   = src/main.o

CFLAGS   = -O2 -G0 -Wall -ffast-math
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)
LIBS     = -lpspaudio -lpspdisplay -lpspctrl -lpspge -lpsppower -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = DUSK CRUSHER

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# Zips a ready-to-copy PSP/GAME/DUSKCRUSHER folder
package: EBOOT.PBP
	rm -rf dist && mkdir -p dist/PSP/GAME/DUSKCRUSHER
	cp EBOOT.PBP dist/PSP/GAME/DUSKCRUSHER/
	cp -r assets dist/PSP/GAME/DUSKCRUSHER/
	cd dist && zip -r ../DUSKCRUSHER_PSP.zip PSP
