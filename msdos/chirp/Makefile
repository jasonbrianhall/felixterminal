# Felix Chirp for MS-DOS (DJGPP, 32-bit DPMI): CHIRP.EXE
#
#   make            CHIRP.EXE, building the libraries first if they aren't
#                   there yet (deps/: FreeType, libwebp, libxmp, SDL3, dr_libs,
#                   stb_vorbis; see build-deps.sh)
#   make deps       just the libraries
#   make dist       build/dist/CHIRP: CHIRP.EXE and README.TXT, for a DOS disk
#   make clean      the program's objects and CHIRP.EXE
#   make distclean  the libraries too
#
# Needs a DJGPP cross compiler (i586-pc-msdosdjgpp-g++), found as FLT's
# Makefile finds it (on the PATH, DJGPP=dir, ~/djgpp, /opt/djgpp,
# /usr/local/djgpp; ../build-djgpp.sh builds one); or, when there is none,
# Docker, with FLT's image (../Dockerfile: djfdyuruiry/djgpp plus git and
# upx). Force either with DOCKER=1 / DOCKER=0. Plus make and git. SDL3 with
# its DOS port is in ../sdl3/ (shared with FLT), CWSDPMI's stub in cwsdpmi/
# (see ../sdl3/README.sdl3). With upx there, the EXE is packed; CWSDPMI's stub in
# front gives it a DPMI host of its own on plain DOS (vmdos has one).
#
# CHIRP.EXE runs under vmdos (github.com/jasonbrianhall/vmdos) or any DOS
# with VESA. See README.md.

DEPS    ?= deps
BUILD   := build
TOP     := ../..

CROSS ?= i586-pc-msdosdjgpp-
ifndef DJGPP
ifeq ($(shell command -v $(CROSS)g++ 2>/dev/null),)
DJGPP := $(firstword $(foreach d,$(HOME)/djgpp /opt/djgpp /usr/local/djgpp,$(if $(wildcard $(d)/bin/$(CROSS)g++),$(d))))
endif
endif
ifdef DJGPP
export PATH := $(DJGPP)/bin:$(PATH)
endif
ifeq ($(origin DOCKER),undefined)
DOCKER := $(if $(shell PATH="$(PATH)" command -v $(CROSS)g++ 2>/dev/null),0,1)
endif

DJGPP_BASE  = djfdyuruiry/djgpp
DJGPP_IMAGE = felixterminal-djgpp
USER_ID  = $(shell id -u)
GROUP_ID = $(shell id -g)

all: CHIRP.EXE

ifeq ($(DOCKER),1)
# ---------------------------------------------------------------- Docker
# felixterminal is mounted; everything that needs the
# compiler runs in the container, as make DOCKER=0 there.
RUN = docker run --rm \
        -v $(abspath $(TOP)):/src:z \
        -w /src/msdos/chirp -u $(USER_ID):$(GROUP_ID) -e HOME=/tmp \
        $(DJGPP_IMAGE) /bin/sh -c

docker-image:
	docker pull $(DJGPP_BASE)
	docker build -t $(DJGPP_IMAGE) $(TOP)/msdos

deps: | docker-image
	$(RUN) "make DOCKER=0 DEPS=$(DEPS) deps"

CHIRP.EXE: FORCE | docker-image
	$(RUN) "make DOCKER=0 DEPS=$(DEPS) CHIRP.EXE"

FORCE:
.PHONY: docker-image FORCE

else
# ---------------------------------------------------------------- DJGPP
CXX      = $(CROSS)g++
CC       = $(CROSS)gcc
UPX     ?= $(shell PATH="$(PATH)" command -v upx 2>/dev/null)

COMMON   = -O2 -march=i586 -Iinclude -I$(TOP) -I$(DEPS)/include -I$(DEPS)/include/freetype2 $(SDL3_INCLUDE)
CXXFLAGS = $(COMMON) -std=gnu++17 -Wno-write-strings -Wno-deprecated-declarations -Wno-format-truncation \
           -include chirp_compat.h -DUSESSH
CFLAGS   = $(COMMON) -w
LIBS     = -L$(DEPS)/lib -lwebp -lxmp -lfreetype $(SDL3_LIB) -lm

# felixchirp/, unchanged (but for HAVE_GSTREAMER off on __DJGPP__)
CHIRP    = felixchirp cdg fc_file fc_text fc_input fc_zip fc_kfn kfn
MINIZ    = miniz miniz_tdef miniz_tinfl miniz_zip
# felixterminal's own MIDI (OPL3) and AIFF renderers, for the mixer
AUDIO    = midi_render dbopl dbopl_wrapper instruments aiff_render
OBJS     = $(BUILD)/chirp_dos.o $(BUILD)/mixer_dos.o $(BUILD)/aes_dos.o $(BUILD)/clock_dos.o $(BUILD)/bigmem_dos.o \
           $(CHIRP:%=$(BUILD)/fc/%.o) $(MINIZ:%=$(BUILD)/fc/%.o) $(AUDIO:%=$(BUILD)/%.o)
STAMP    = $(DEPS)/.built
HDRS     = $(wildcard include/*.h include/*/*.h) $(TOP)/felixchirp/felixchirp.h

# SDL3 with its DOS port: ../sdl3 (SDLPoPDOS's, see ../sdl3/README.sdl3)
include ../sdl3/sdl3.mk

$(STAMP):
	DEPS=$(abspath $(DEPS)) sh build-deps.sh
deps: $(STAMP) $(SDL3_LIB)

CHIRP.EXE: $(OBJS) $(SDL3_LIB)
	$(CXX) -s -Wl,--wrap=uclock -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free $(OBJS) $(LIBS) -o $(BUILD)/chirp.exe
	rm -f $@ $(BUILD)/packed.exe
	$(if $(UPX),$(UPX) -q --best -o $(BUILD)/packed.exe $(BUILD)/chirp.exe > /dev/null,cp $(BUILD)/chirp.exe $(BUILD)/packed.exe)
	@# CWSDPMI's stub in front (cwsdpmi/), as vmdos's Felix BASIC: the
	@# DPMI host that's running, else CWSDPMI.
	@if [ -f cwsdpmi/CWSDSTUB.EXE ]; then \
	    set -- $$(od -An -tu2 -j2 -N4 $(BUILD)/packed.exe); \
	    skip=$$(( $$2 * 512 - ($$1 ? 512 - $$1 : 0) )); \
	    { cat cwsdpmi/CWSDSTUB.EXE; tail -c +$$((skip + 1)) $(BUILD)/packed.exe; } > $@; \
	else cp $(BUILD)/packed.exe $@; fi
	@echo "Created CHIRP.EXE"

# felixchirp.h has to leave GStreamer out on __DJGPP__ (the felixchirp/
# part of the commit that added msdos/chirp): no gst/gst.h on DOS.
$(BUILD)/fc/.checked: $(TOP)/felixchirp/felixchirp.h | $(BUILD)/fc
	@grep -q '!defined(__DJGPP__)' $< || { echo "$<: GStreamer isn't left out on DOS (no '!defined(__DJGPP__)'): felixchirp/ is older than msdos/chirp" >&2; exit 1; }
	@touch $@

$(OBJS): $(STAMP) $(HDRS) $(BUILD)/fc/.checked | $(BUILD)/fc
$(BUILD)/chirp_dos.o: chirp_dos.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/mixer_dos.o: mixer_dos.cpp
	$(CXX) $(CXXFLAGS) -w -c $< -o $@
$(BUILD)/aes_dos.o: aes_dos.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/clock_dos.o: clock_dos.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/bigmem_dos.o: bigmem_dos.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/fc/%.o: $(TOP)/felixchirp/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/fc/%.o: $(TOP)/felixchirp/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(TOP)/%.cpp
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

$(BUILD)/fc:
	mkdir -p $@
endif

# ---------------------------------------------------------------- packaging
dist: CHIRP.EXE
	rm -rf $(BUILD)/dist && mkdir -p $(BUILD)/dist/CHIRP
	cp CHIRP.EXE $(BUILD)/dist/CHIRP/CHIRP.EXE
	sed 's/$$/\r/' README.md > $(BUILD)/dist/CHIRP/README.TXT
	@echo "Created $(BUILD)/dist/CHIRP"

clean:
	rm -rf $(BUILD) CHIRP.EXE

distclean: clean
	rm -rf $(DEPS)

.PHONY: all deps dist clean distclean
