# Felix BASIC for MS-DOS / FreeDOS (DJGPP, 32-bit DPMI)
#
#   make            felixbas.exe with CWSDPMI embedded (runs on its own)
#   make msdos_original
#                   felixbas.exe that needs CWSDPMI.EXE next to it (or any
#                   other DPMI host: Windows 9x, HDPMI32, ...)
#   make run        run it in DOSBox, with the sample programs
#   make clean
#
# The compiler: a local DJGPP cross compiler (i586-pc-msdosdjgpp-g++ on the
# PATH) when there is one, otherwise the djfdyuruiry/djgpp Docker image.
# Force either with DOCKER=1 / DOCKER=0.
#
# The interpreter (../main.cpp, vars, expr, program, commands, basic_print)
# is compiled unchanged with the SDL build's graphics on (USE_SDL_WINDOW),
# as the bare-metal build does, and basic_gfx_dos.cpp (the SDL display
# code with the window and FreeType swapped for the framebuffer and the VGA
# font, from ../baremetal) draws on the screen. The SDL stand-in in
# ../baremetal/overrides supplies the SDL types and clock.
# dos_platform.cpp replaces the bare-metal kernel (VESA
# video, BIOS keyboard, INT 33h mouse, PIT clock); dos_sound.cpp plays
# SOUND/PLAY/BEEP on the PC speaker.
#
# Runs on a 386 or later; without a 387, put EMU387.DXE (DJGPP v2misc)
# next to the program. Needs a VESA BIOS for 640x480 in more than 16
# colours (FELIXVID=800x600x16 picks another mode); plain VGA gets mode 12h.
VERSION = 20261022

SRC     := ..
BM      := ../baremetal
BUILD   := build
TESTS   := $(SRC)/test

DOS_TARGET      = felixbas.exe
DOS_COFF        = felixbas
COFF_TARGET     = felixbas.coff
EMBEDDED_TARGET = embedded.exe

DJGPP_IMAGE = djfdyuruiry/djgpp
CSDPMI_URL  = http://na.mirror.garr.it/mirrors/djgpp/current/v2misc/csdpmi7b.zip

USER_ID  = $(shell id -u)
GROUP_ID = $(shell id -g)

CROSS ?= i586-pc-msdosdjgpp-
ifeq ($(origin DOCKER),undefined)
DOCKER := $(if $(shell command -v $(CROSS)g++ 2>/dev/null),0,1)
endif

ifeq ($(DOCKER),1)
# The whole of wopr_basic is mounted, so ../ and ../baremetal are reachable.
RUN      = docker run --rm -v $(abspath $(SRC)):/src:z -w /src/msdos -u $(USER_ID):$(GROUP_ID) $(DJGPP_IMAGE) /bin/sh -c
CXX_DOS  = g++
EXE2COFF = exe2coff
else
RUN      = /bin/sh -c
CXX_DOS  = $(CROSS)g++
EXE2COFF = $(shell command -v $(CROSS)exe2coff 2>/dev/null || echo $(dir $(shell command -v $(CROSS)g++))../i586-pc-msdosdjgpp/bin/exe2coff)
endif

# As the 32-bit bare-metal build: a 386 with a 387, doubles for numbers.
LIMITS   = -DMAX_VARS=1024 -DMAX_ARRAY_SIZE=1048576 -DCTRL_STACK_MAX=1024 -DMAX_VARNAME=64 -DMAX_DIR_ENTRIES=512
CXXFLAGS = -O2 -march=i386 -mtune=i586 -std=gnu++17 -fno-rtti -fpermissive -w \
           -include dos_compat.h -I. -I$(BM)/overrides -I$(BM) -I$(SRC) \
           -DUSE_SDL_WINDOW -DBASIC_BAREMETAL -DDONTUSEGMP $(LIMITS)
LDFLAGS  = -s -Wl,--gc-sections

CORE_SRCS = $(addprefix $(SRC)/,main.cpp vars.cpp expr.cpp program.cpp commands.cpp basic_print.cpp)
DOS_SRCS  = $(CORE_SRCS) basic_gfx_dos.cpp $(BM)/sound_bm.cpp dos_platform.cpp dos_sound.cpp

CORE_HDRS = $(wildcard $(SRC)/*.h) $(wildcard $(BM)/*.hpp) $(BM)/font.h $(BM)/overrides/SDL2/SDL.h dos_compat.h

all: msdos_embedded

pull-djgpp:
ifeq ($(DOCKER),1)
	docker pull $(DJGPP_IMAGE)
endif

get-csdpmi:
	@if [ ! -d "csdpmi" ]; then \
		wget $(CSDPMI_URL); \
		unzip -o csdpmi7b.zip -d csdpmi; \
	fi

$(COFF_TARGET): $(DOS_SRCS) $(CORE_HDRS) | pull-djgpp
	$(RUN) "$(CXX_DOS) $(CXXFLAGS) $(DOS_SRCS) $(LDFLAGS) -o $(DOS_TARGET) && \
		$(EXE2COFF) $(DOS_TARGET) && mv $(DOS_COFF) $(COFF_TARGET)"

# Needs CWSDPMI.EXE (or another DPMI host) at run time.
msdos_original: $(COFF_TARGET) get-csdpmi
	$(RUN) "$(CXX_DOS) $(CXXFLAGS) $(DOS_SRCS) $(LDFLAGS) -o $(DOS_TARGET)"
	cp csdpmi/bin/CWSDPMI.EXE .

# CWSDPMI's stub in front: one file that runs on plain DOS.
msdos_embedded: $(COFF_TARGET) get-csdpmi
	@if [ ! -f "csdpmi/bin/CWSDSTUB.EXE" ]; then \
		echo "ERROR: CWSDSTUB.EXE not found in csdpmi/bin directory!"; \
		echo "Make sure you have the complete CSDPMI package."; \
		exit 1; \
	fi
	cat csdpmi/bin/CWSDSTUB.EXE $(COFF_TARGET) > $(EMBEDDED_TARGET)
	mv $(EMBEDDED_TARGET) $(DOS_TARGET)
	chmod +x $(DOS_TARGET)
	@echo "Created $(DOS_TARGET) with embedded DPMI extender"

msdos: msdos_embedded

# DOSBox with this folder as C: and the sample programs copied in.
run: msdos_embedded
	@mkdir -p $(BUILD)/dosc
	cp $(DOS_TARGET) $(BUILD)/dosc/
	-cp $(TESTS)/*.bas $(TESTS)/*.BAS $(BUILD)/dosc/ 2>/dev/null
	printf '[cpu]\ncore=dynamic\ncputype=auto\ncycles=max\n[dosbox]\nmachine=svga_s3\nmemsize=32\n' > $(BUILD)/dosbox.conf
	dosbox -conf $(BUILD)/dosbox.conf -c "mount c $(BUILD)/dosc" -c "c:" -c "$(DOS_TARGET)"

clean:
	rm -rf $(BUILD) $(DOS_TARGET) $(COFF_TARGET) $(EMBEDDED_TARGET) $(DOS_COFF) CWSDPMI.EXE || true
	rm -rf csdpmi csdpmi7b.zip csdpmi7b.zip.* || true

.PHONY: all pull-djgpp get-csdpmi msdos msdos_original msdos_embedded run clean
