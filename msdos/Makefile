# Felix Terminal for MS-DOS (DJGPP, 32-bit DPMI): FLT.EXE
#
#   make            FLT.EXE, building the libraries first if they aren't
#                   there yet (deps/: zlib, libpng, FreeType, OpenSSL,
#                   libssh2, SDL3, vmdos's sockets; see build-deps.sh)
#   make deps       just the libraries
#   make dist       build/dist/FLT: FLT.EXE and README.TXT, for a DOS disk
#   make clean      the program's objects and FLT.EXE
#   make distclean  the libraries too
#
# Needs a DJGPP cross compiler (i586-pc-msdosdjgpp-g++): the one FLT.EXE is
# built and tested with comes from `sh build-djgpp.sh` (gcc 14.2.0; into
# ~/djgpp, found there), or any on the PATH or DJGPP=dir; or Docker, which gets the djfdyuruiry/djgpp
# image instead (with git, python3, perl, a host gcc and upx added on top:
# Dockerfile). Force either with DOCKER=1 / DOCKER=0. Plus make, and
# checkouts of vmdos and SDLPoPDOS next to this one (or VMDOS=dir,
# SDLDOS=dir): vmdos's OpenSSL build and sockets, SDLPoPDOS's SDL3 with its
# DOS port.
#
# FLT.EXE runs under vmdos (github.com/jasonbrianhall/vmdos), on its TCP/IP.
# See README.md.

VMDOS   ?= ../../vmdos
SDLDOS  ?= ../../SDLPoPDOS/msdos
DEPS    ?= deps
BUILD   := build
TOP     := ..

# The compiler: a local DJGPP cross compiler when there is one (on the PATH,
# DJGPP=dir, or where build-djgpp.sh puts it: ~/djgpp, /opt/djgpp,
# /usr/local/djgpp), otherwise Docker.
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

all: FLT.EXE

ifeq ($(DOCKER),1)
# ---------------------------------------------------------------- Docker
# The felixterminal tree, vmdos and SDLPoPDOS are mounted; everything that
# needs the compiler runs in the container, as make DOCKER=0 there. HOME is
# somewhere writable for git.
RUN = docker run --rm \
        -v $(abspath $(TOP)):/src:z -v $(abspath $(VMDOS)):/vmdos:z -v $(abspath $(SDLDOS)):/sdldos:z \
        -w /src/msdos -u $(USER_ID):$(GROUP_ID) -e HOME=/tmp \
        $(DJGPP_IMAGE) /bin/sh -c

docker-image:
	docker pull $(DJGPP_BASE)
	docker build -t $(DJGPP_IMAGE) .

deps: | docker-image
	$(RUN) "make DOCKER=0 VMDOS=/vmdos SDLDOS=/sdldos DEPS=$(DEPS) deps"

FLT.EXE: FORCE | docker-image
	$(RUN) "make DOCKER=0 VMDOS=/vmdos SDLDOS=/sdldos DEPS=$(DEPS) FLT.EXE"

FORCE:
.PHONY: docker-image FORCE

else
# ---------------------------------------------------------------- DJGPP
CXX      = $(CROSS)g++
UPX     ?= $(shell PATH="$(PATH)" command -v upx 2>/dev/null)

CXXFLAGS = -O2 -march=i586 -std=gnu++17 -Wno-write-strings -Wno-deprecated-declarations \
           -include dos_compat.h -Iinclude -I$(TOP) -I$(DEPS)/include -I$(DEPS)/include/freetype2 -DUSESSH
# ssh_session.cpp: sockets by Watt-32's headers (vmdos's sockets underneath)
SSH_CXXFLAGS = -DWATT32 -DWATT32_NO_OLDIES -I$(DEPS)/src/Watt-32/inc
LIBS     = -L$(DEPS)/lib -lssh2 -lssl -lcrypto -lvmdosnet -lpng -lfreetype -lz -lSDL3 -lm

# felixterminal's own files, unchanged but for a few #ifdef __DJGPP__ in
# ssh_session.cpp; flt_dos.cpp is the DOS front end.
SHARED   = terminal term_color sixel_graphics kitty_graphics ssh_session
OBJS     = $(BUILD)/flt_dos.o $(SHARED:%=$(BUILD)/%.o)
STAMP    = $(DEPS)/.built

# The libraries: once (build-deps.sh leaves $(STAMP) when it's done).
$(STAMP):
	VMDOS=$(abspath $(VMDOS)) SDLDOS=$(abspath $(SDLDOS)) DEPS=$(abspath $(DEPS)) sh build-deps.sh
deps: $(STAMP)

FLT.EXE: $(OBJS)
	$(CXX) -s $(OBJS) $(LIBS) -o $(BUILD)/flt.exe
	rm -f $@
	$(if $(UPX),$(UPX) -q --best -o $@ $(BUILD)/flt.exe > /dev/null,cp $(BUILD)/flt.exe $@)
	@echo "Created FLT.EXE: copy it to vmdos's C: (C:\VMDOS\NET is on the PATH)"

$(OBJS): $(STAMP) dos_compat.h include/GL/glew.h include/SDL2/SDL.h | $(BUILD)
$(BUILD)/flt_dos.o: flt_dos.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/ssh_session.o: $(TOP)/ssh_session.cpp
	$(CXX) $(CXXFLAGS) $(SSH_CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(TOP)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@
endif

# ---------------------------------------------------------------- packaging
# The folder to copy to a DOS machine (or vmdos's C:)
dist: FLT.EXE
	rm -rf $(BUILD)/dist && mkdir -p $(BUILD)/dist/FLT
	cp FLT.EXE $(BUILD)/dist/FLT/FLT.EXE
	sed 's/$$/\r/' README.md > $(BUILD)/dist/FLT/README.TXT
	@echo "Created $(BUILD)/dist/FLT"

clean:
	rm -rf $(BUILD) FLT.EXE

distclean: clean
	rm -rf $(DEPS)

.PHONY: all deps dist clean distclean
