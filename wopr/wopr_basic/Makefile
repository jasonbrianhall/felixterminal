# --------------------------------------------------------------------
# Toolchain selection
#   make                       → native Linux       (output: build/linux/felixbasic)
#   make windows               → Windows cross      (output: build/windows/felixbasic.exe)
#   make sdlwindow             → Linux SDL GUI      (output: build/linux/felixbasic_sdlwin)
#   make sdlwindow TARGET=windows → Windows SDL GUI (output: build/windows/felixbasic_sdlwin.exe)
#   make DONTUSEGMP=1          → build without GMP (uses double fallback)
#   make help                  → show this help
# --------------------------------------------------------------------
ifeq ($(TARGET),windows)
    CC         = x86_64-w64-mingw32-g++
    PKG_CONFIG = mingw64-pkg-config
    BUILDDIR   = build/windows
    EXE        = $(BUILDDIR)/felixbasic.exe
    CFLAGS     = -O2 -Wall -std=c++11 -DWIN32 -D_WIN32
    LDFLAGS    = -lm -mconsole
    SDL_WIN_EXE = $(BUILDDIR)/felixbasic_sdlwin.exe
else
    CC         = g++
    PKG_CONFIG = pkg-config
    BUILDDIR   = build/linux
    EXE        = $(BUILDDIR)/felixbasic
    CFLAGS     = -O2 -Wall -std=c++11
    LDFLAGS    = -lm
    SDL_WIN_EXE = $(BUILDDIR)/felixbasic_sdlwin
endif

# GMP support (default: enabled; disable with DONTUSEGMP=1)
ifndef DONTUSEGMP
    CFLAGS += -DHAVE_GMP
    LDFLAGS += -lgmp
    $(info Building with GMP arbitrary-precision arithmetic)
else
    CFLAGS += -DDONTUSEGMP
    $(info Building WITHOUT GMP — using double precision fallback)
endif

# --------------------------------------------------------------------
# SDL2 autodetection
# --------------------------------------------------------------------
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs   sdl2 2>/dev/null)

ifneq ($(SDL_LIBS),)
    CFLAGS    += $(SDL_CFLAGS) -DHAVE_SDL -fpermissive
    LDFLAGS   += $(SDL_LIBS)
    SOUND_SRC  = sound_sdl.cpp
    $(info SDL2 found — building with audio support)
else
    SOUND_SRC  = sound_null.cpp
    $(info SDL2 not found — building without audio support)
endif

# --------------------------------------------------------------------
# FreeType autodetection (required for the SDL window build only)
# --------------------------------------------------------------------
FT_CFLAGS := $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
FT_LIBS   := $(shell $(PKG_CONFIG) --libs   freetype2 2>/dev/null)

# --------------------------------------------------------------------
# INLINE BASIC EMBEDDING
# --------------------------------------------------------------------
ifdef INLINEBASIC
    CFLAGS += -DINLINEBASIC
    BASIC_HEADER = inline_basic.h
    GENERATED_HEADERS = $(BASIC_HEADER)

$(BASIC_HEADER): $(INLINEBASIC)
	xxd -n wopr_basic_program -i $< > $@
	$(info Embedding BASIC program: $<)
endif

# --------------------------------------------------------------------
# SOURCES → per-target object paths
# --------------------------------------------------------------------
SRCS = main.cpp vars.cpp expr.cpp program.cpp commands.cpp editor.cpp display_ansi.cpp basic_print.cpp $(SOUND_SRC)
OBJS = $(patsubst %.cpp,$(BUILDDIR)/%.o,$(SRCS))

# SDL window build uses basic_gfx_sdl.cpp instead of display_ansi.cpp
SDL_WIN_SRCS = main.cpp vars.cpp expr.cpp program.cpp commands.cpp editor.cpp basic_gfx_sdl.cpp basic_print.cpp $(SOUND_SRC)
SDL_WIN_OBJS = $(patsubst %.cpp,$(BUILDDIR)/sdlwin/%.o,$(SDL_WIN_SRCS))

# --------------------------------------------------------------------
# BUILD TARGETS
# --------------------------------------------------------------------
.PHONY: all linux windows sdlwindow clean embed help

all: linux

# --------------------------------------------------------------------
# HELP
# --------------------------------------------------------------------
help:
	@echo ""
	@echo "Usage: make [TARGET=windows] [DONTUSEGMP=1] <target>"
	@echo ""
	@echo "Targets:"
	@echo "  (default) / linux        Build native Linux console binary"
	@echo "                             Output: build/linux/felixbasic"
	@echo ""
	@echo "  windows                  Cross-compile Windows console binary"
	@echo "                             Requires: x86_64-w64-mingw32-g++"
	@echo "                             Output: build/windows/felixbasic.exe"
	@echo ""
	@echo "  sdlwindow                Build SDL2 GUI window (Linux)"
	@echo "                             Requires: libsdl2-dev, libfreetype6-dev"
	@echo "                             Output: build/linux/felixbasic_sdlwin"
	@echo ""
	@echo "  sdlwindow TARGET=windows Cross-compile SDL2 GUI window (Windows)"
	@echo "                             Requires: mingw64 SDL2/FreeType packages"
	@echo "                             Output: build/windows/felixbasic_sdlwin.exe"
	@echo ""
	@echo "  embed FILE=<file.bas>    Embed a BASIC program inline at compile time"
	@echo "                             Combine with any build target, e.g.:"
	@echo "                             make embed FILE=myprog.bas"
	@echo ""
	@echo "  clean                    Remove all build artifacts and generated headers"
	@echo ""
	@echo "Optional variables:"
	@echo "  TARGET=windows           Cross-compile for Windows (use with any target)"
	@echo "  DONTUSEGMP=1             Build without GMP (uses double precision fallback)"
	@echo "  FILE=<path>              BASIC source file to embed (used with 'embed')"
	@echo "  INLINEBASIC=<path>       Alias for FILE when invoking embed directly"
	@echo ""

linux:
	$(MAKE) build/linux/felixbasic TARGET=linux

windows:
	$(MAKE) build/windows/felixbasic.exe TARGET=windows

# SDL window build — direct SDL2 rendering, no OSC escape codes
# Requires SDL2 + FreeType.
# Linux output:   build/linux/felixbasic_sdlwin
# Windows output: build/windows/felixbasic_sdlwin.exe  (use TARGET=windows)
sdlwindow:
ifeq ($(FT_LIBS),)
	$(error FreeType not found — install libfreetype6-dev (Linux) or the mingw64 FreeType package (Windows cross))
endif
ifeq ($(SDL_LIBS),)
	$(error SDL2 not found — install libsdl2-dev (Linux) or the mingw64 SDL2 package (Windows cross))
endif
	$(MAKE) $(SDL_WIN_EXE) TARGET=$(TARGET)

$(BUILDDIR)/felixbasic_sdlwin $(BUILDDIR)/felixbasic_sdlwin.exe: $(SDL_WIN_OBJS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -DUSE_SDL_WINDOW $(FT_CFLAGS) -o $@ $(SDL_WIN_OBJS) $(LDFLAGS) $(FT_LIBS)

$(EXE): $(GENERATED_HEADERS) $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

embed:
	$(MAKE) $(EXE) INLINEBASIC=$(FILE)

# --------------------------------------------------------------------
# OBJECT RULES
# --------------------------------------------------------------------
$(BUILDDIR)/%.o: %.cpp basic.h display.h sound.h $(GENERATED_HEADERS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# SDL window objects — compiled with USE_SDL_WINDOW + FreeType flags
$(BUILDDIR)/sdlwin/%.o: %.cpp basic.h display.h sound.h basic_gfx.h basic_gfx_sdl.h $(GENERATED_HEADERS)
	@mkdir -p $(BUILDDIR)/sdlwin
	$(CC) $(CFLAGS) -DUSE_SDL_WINDOW $(FT_CFLAGS) -c -o $@ $<

# --------------------------------------------------------------------
# CLEAN
# --------------------------------------------------------------------
clean:
	rm -rf build
	rm -f inline_basic.h
