# Felix Terminal Makefile
# Cross-compile for Windows with: make windows
# Native Linux build with:        make linux
#
# SSH/SFTP support (libssh2) is always built in.

# Compiler settings
CXX_LINUX = g++
CXX_WIN   = x86_64-w64-mingw32-g++
CC_LINUX  = gcc
CC_WIN    = x86_64-w64-mingw32-gcc

# Common flags
CXXFLAGS_COMMON = -Wall -Wextra -std=c++17 -DWOPR
CFLAGS_COMMON   = -Wall -Wextra

# Package config
PKG_CONFIG_LINUX = pkg-config
PKG_CONFIG_WIN   = mingw64-pkg-config

# ============================================================================
# FREETYPE
# ============================================================================
FREETYPE_CFLAGS_LINUX := $(shell $(PKG_CONFIG_LINUX) --cflags freetype2 2>/dev/null || echo "-I/usr/include/freetype2")
FREETYPE_LIBS_LINUX   := $(shell $(PKG_CONFIG_LINUX) --libs   freetype2 2>/dev/null || echo "-lfreetype")

FREETYPE_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags freetype2 2>/dev/null || echo "")
FREETYPE_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   freetype2 2>/dev/null || echo "-lfreetype")

# ============================================================================
# SDL2
# ============================================================================
SDL2_CFLAGS_LINUX := $(shell pkg-config --cflags sdl2 2>/dev/null || echo "")
SDL2_LIBS_LINUX   := $(shell pkg-config --libs   sdl2 2>/dev/null || echo "-lSDL2")

SDL2_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags sdl2 2>/dev/null || echo "")
SDL2_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   sdl2 2>/dev/null || echo "-lSDL2")

# ============================================================================
# GLEW
# ============================================================================
GLEW_CFLAGS_LINUX := $(shell pkg-config --cflags glew 2>/dev/null || echo "")
GLEW_LIBS_LINUX   := $(shell pkg-config --libs   glew 2>/dev/null || echo "-lGLEW")

GLEW_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags glew 2>/dev/null || echo "")
GLEW_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   glew 2>/dev/null || echo "-lglew32")

# ============================================================================
# LIBSSH2  (SSH/SFTP — always built in)
# ============================================================================
SSH2_CFLAGS_LINUX := $(shell $(PKG_CONFIG_LINUX) --cflags libssh2 2>/dev/null || echo "")
SSH2_LIBS_LINUX   := $(shell $(PKG_CONFIG_LINUX) --libs   libssh2 2>/dev/null || echo "-lssh2")

SSH2_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags libssh2 2>/dev/null || echo "")
SSH2_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   libssh2 2>/dev/null || echo "-lssh2")

# ============================================================================
# FFMPEG/LIBAV  (for M4A, WMA, MP2 audio conversion on Linux)
# ============================================================================
FFMPEG_CFLAGS_LINUX := $(shell $(PKG_CONFIG_LINUX) --cflags libavformat libavcodec libavutil libswresample 2>/dev/null || echo "")
FFMPEG_LIBS_LINUX   := $(shell $(PKG_CONFIG_LINUX) --libs   libavformat libavcodec libavutil libswresample 2>/dev/null || echo "-lavformat -lavcodec -lavutil -lswresample")

# ============================================================================
# GSTREAMER  (for video playback)
# ============================================================================
GSTREAMER_CFLAGS_LINUX := $(shell $(PKG_CONFIG_LINUX) --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null || echo "")
GSTREAMER_LIBS_LINUX   := $(shell $(PKG_CONFIG_LINUX) --libs   gstreamer-1.0 gstreamer-app-1.0 2>/dev/null || echo "-lgstreamer-1.0 -lgstapp-1.0")

GSTREAMER_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null || echo "")
GSTREAMER_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   gstreamer-1.0 gstreamer-app-1.0 2>/dev/null || echo "-lgstreamer-1.0 -lgstapp-1.0")

# ============================================================================
# wxWIDGETS (for FelixTerminalGUI frontend)
# ============================================================================
WX_CFLAGS_LINUX := $(shell wx-config --cppflags 2>/dev/null || echo "")
WX_LIBS_LINUX   := $(shell wx-config --libs 2>/dev/null || echo "-lwx_gtk3u_core-3.0 -lwx_baseu-3.0")

WX_CFLAGS_WIN   := -I/usr/x86_64-w64-mingw32/sys-root/mingw/include/wx-3.0 \
                   -I/usr/x86_64-w64-mingw32/sys-root/mingw/lib/wx/include/x86_64-w64-mingw32-msw-unicode-3.0
WX_LIBS_WIN     := -L/usr/x86_64-w64-mingw32/sys-root/mingw/lib \
                   -lwx_mswu_core-3.0-x86_64-w64-mingw32 \
                   -lwx_baseu-3.0-x86_64-w64-mingw32

# ============================================================================
# FRONTEND BUILD TARGETS
# ============================================================================
FRONTEND_LINUX      = $(BUILD_DIR_LINUX)/FelixTerminalGUI
FRONTEND_WIN        = $(BUILD_DIR_WIN)/FelixTerminalGUI.exe

# SSH (always enabled)
  SSH_SRCS         = ssh_session.cpp port_forward.cpp pf_overlay.cpp
  SSH_DEFINE       = -DUSESSH
  SSH_CFLAGS_LINUX = $(SSH2_CFLAGS_LINUX)
  SSH_LIBS_LINUX   = $(SSH2_LIBS_LINUX) -lcrypto -lssl
  SSH_CFLAGS_WIN   = $(SSH2_CFLAGS_WIN)
  SSH_LIBS_WIN     = $(SSH2_LIBS_WIN) -lcrypto -lssl -lws2_32
  SSH_SUFFIX       = 

# ============================================================================
# AUDIO CODEC LIBS  (auto-detected — linked in statically to avoid dlopen)
# Each lib is only added to LDFLAGS if pkg-config finds it.
# Linux:   sudo apt install libxmp-dev libmpg123-dev libopus-dev libopusfile-dev
#                           libvorbis-dev libflac-dev
#          sudo dnf install libxmp-devel mpg123-devel opus-devel opusfile-devel
#                           libvorbis-devel flac-devel
# Windows: equivalent mingw64-* packages or via mxe.cc
# ============================================================================
pkg_lib_linux = $(shell $(PKG_CONFIG_LINUX) --exists $(1) 2>/dev/null && $(PKG_CONFIG_LINUX) --libs $(1) || echo "")
pkg_lib_win   = $(shell $(PKG_CONFIG_WIN)   --exists $(1) 2>/dev/null && $(PKG_CONFIG_WIN)   --libs $(1) || echo "")

CODEC_LIBS_LINUX := $(call pkg_lib_linux,libxmp) \
                    $(call pkg_lib_linux,libmpg123) \
                    $(call pkg_lib_linux,opusfile) \
                    $(call pkg_lib_linux,opus) \
                    $(call pkg_lib_linux,vorbisfile) \
                    $(call pkg_lib_linux,flac)

CODEC_LIBS_WIN   := $(call pkg_lib_win,libxmp) \
                    $(call pkg_lib_win,libmpg123) \
                    $(call pkg_lib_win,opusfile) \
                    $(call pkg_lib_win,opus) \
                    $(call pkg_lib_win,vorbisfile) \
                    $(call pkg_lib_win,flac)

# ============================================================================
# MSIX BUILD  (opt-in via FELIXBASIC=1)
# Adds -DFELIXBASIC_BUILD to Windows flags so #ifdef FELIXBASIC_BUILD can swap
# cmd.exe for felixbasic.exe without needing runtime parameters.
# ============================================================================
ifeq ($(FELIXBASIC),1)
  FELIXBASIC_DEFINE = -DFELIXBASIC_BUILD
else
  FELIXBASIC_DEFINE =
endif

# ============================================================================
# FRONTEND FLAGS
# ============================================================================
CXXFLAGS_FRONTEND_LINUX = -std=c++14 $(WX_CFLAGS_LINUX) -O2
LDFLAGS_FRONTEND_LINUX  = $(WX_LIBS_LINUX) -lm

CXXFLAGS_FRONTEND_WIN   = -std=c++17 $(WX_CFLAGS_WIN) -O2
LDFLAGS_FRONTEND_WIN    = $(WX_LIBS_WIN) -lm

# ============================================================================
# LINUX FLAGS
# ============================================================================
CXXFLAGS_LINUX = $(CXXFLAGS_COMMON) \
                 $(SDL2_CFLAGS_LINUX) $(GLEW_CFLAGS_LINUX) $(FREETYPE_CFLAGS_LINUX) \
                 $(GSTREAMER_CFLAGS_LINUX) $(FFMPEG_CFLAGS_LINUX) \
                 $(SSH_CFLAGS_LINUX) \
                 -Iwopr \
                 -DLINUX $(SSH_DEFINE) -O2 -ffunction-sections -fdata-sections -flto -DDONTUSEGMP

LDFLAGS_LINUX  = $(SDL2_LIBS_LINUX) $(GLEW_LIBS_LINUX) $(FREETYPE_LIBS_LINUX) \
                 $(GSTREAMER_LIBS_LINUX) $(FFMPEG_LIBS_LINUX) \
                 $(SSH_LIBS_LINUX) \
                 -lGL -lpng -lz -lm -pthread -lstdc++ -lSDL2_mixer $(CODEC_LIBS_LINUX) \
                 -lssl -lcrypto \
                 -s -Wl,--gc-sections -flto -lwebp

CXXFLAGS_LINUX_DEBUG = $(CXXFLAGS_COMMON) \
                       $(SDL2_CFLAGS_LINUX) $(GLEW_CFLAGS_LINUX) $(FREETYPE_CFLAGS_LINUX) \
                       $(GSTREAMER_CFLAGS_LINUX) $(FFMPEG_CFLAGS_LINUX) \
                       $(SSH_CFLAGS_LINUX) \
                       -Iwopr \
                       -DLINUX $(SSH_DEFINE) -DDEBUG -g -O0 -fno-omit-frame-pointer \
                       -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=address,undefined \
                       -DDONTUSEGMP

LDFLAGS_LINUX_DEBUG  = $(SDL2_LIBS_LINUX) $(GLEW_LIBS_LINUX) $(FREETYPE_LIBS_LINUX) \
                       $(GSTREAMER_LIBS_LINUX) $(FFMPEG_LIBS_LINUX) \
                       $(SSH_LIBS_LINUX) \
                       -lGL -lpng -lz -lm -pthread -lstdc++ -lSDL2_mixer $(CODEC_LIBS_LINUX) -lssl -lcrypto -lwebp \
                       -fsanitize=address -fsanitize=undefined -rdynamic

# C flags for miniz .c files (no -std=c++17, no -Wextra pedantry on C)
CFLAGS_LINUX       = $(CFLAGS_COMMON) -DLINUX -O2 
CFLAGS_LINUX_DEBUG = $(CFLAGS_COMMON) -DLINUX -DDEBUG -g -O0 -fno-omit-frame-pointer \
                     -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=address,undefined

# ============================================================================
# WINDOWS FLAGS
# ============================================================================
CXXFLAGS_WIN = $(CXXFLAGS_COMMON) \
               $(SDL2_CFLAGS_WIN) $(GLEW_CFLAGS_WIN) $(FREETYPE_CFLAGS_WIN) \
               $(GSTREAMER_CFLAGS_WIN) \
               $(SSH_CFLAGS_WIN) \
               -Iwopr \
               -DWIN32 -D_WIN32 -D_WIN32_WINNT=0x0A00 \
               $(SSH_DEFINE) $(FELIXBASIC_DEFINE) -O2 -ffunction-sections -fdata-sections -flto -DDONTUSEGMP

# term_pty_win.cpp replaces term_pty.cpp for ConPTY
# Media Foundation libs (mf, mfplat, mfreadwrite, mfuuid) for M4A audio conversion on Windows
LDFLAGS_WIN  = $(SDL2_LIBS_WIN) $(GLEW_LIBS_WIN) $(FREETYPE_LIBS_WIN) \
               $(GSTREAMER_LIBS_WIN) \
               $(SSH_LIBS_WIN) \
               -lopengl32 -lpng -lz -lwinmm -lSDL2_mixer $(CODEC_LIBS_WIN) -lshlwapi -lws2_32 \
               -lmf -lmfplat -lmfreadwrite -lmfuuid -lole32 -loleaut32 \
               -Wl,--gc-sections -flto -lwebp -Wl,-Map=$(BUILD_DIR_WIN)/flt.map

CXXFLAGS_WIN_DEBUG = $(CXXFLAGS_COMMON) \
                     $(SDL2_CFLAGS_WIN) $(GLEW_CFLAGS_WIN) $(FREETYPE_CFLAGS_WIN) \
                     $(GSTREAMER_CFLAGS_WIN) \
                     $(SSH_CFLAGS_WIN) \
                     -Iwopr \
                     -DWIN32 -D_WIN32 -D_WIN32_WINNT=0x0A00 \
                     $(SSH_DEFINE) $(FELIXBASIC_DEFINE) -DDEBUG -g -O0 -DDONTUSEGMP

CFLAGS_WIN         = $(CFLAGS_COMMON) -DWIN32 -D_WIN32 -O2
CFLAGS_WIN_DEBUG   = $(CFLAGS_COMMON) -DWIN32 -D_WIN32 -DDEBUG -g -O0

# ============================================================================
# SOURCES
# ============================================================================
SRCS_COMMON = felixterminal.cpp     gl_renderer.cpp       \
              sdl_renderer.cpp      ft_font.cpp           \
              term_color.cpp        terminal.cpp          \
              term_ui.cpp           gl_bouncingcircle.cpp \
              fight_mode.cpp        crt_audio.cpp         \
              felix_settings.cpp    kitty_graphics.cpp    \
              font_manager.cpp      sftp_overlay.cpp      \
              sftp_console.cpp      felixchirp/felixchirp.cpp  \
              felixchirp/cdg.cpp      $(SSH_SRCS)                \
              telnet_session.cpp                               \
              basic_graphics.cpp      mml_player.cpp             \
              glyph_atlas.cpp         sticky_prompt.cpp          \
              ssh_key_manager.cpp     serial_session.cpp         \
              felixchirp/fc_file.cpp  felixchirp/fc_text.cpp    \
              felixchirp/fc_input.cpp felixchirp/fc_zip.cpp    \
              felixchirp/fc_kfn.cpp   felixchirp/kfn.cpp       \
              sftp_webserver.cpp      midi_render.cpp          \
              dbopl.cpp               dbopl_wrapper.cpp        \
              instruments.cpp         voc_render.cpp           \
              au_render.cpp           aiff_render.cpp          \
              convertm4atowavlin.cpp  convertm4atowavwin.cpp   \
              vfs.cpp                 sixel_graphics.cpp       \
              term_clipboard.cpp

SRCS_MINIZ = libtelnet.c felixchirp/miniz.c felixchirp/miniz_tdef.c felixchirp/miniz_tinfl.c felixchirp/miniz_zip.c \
            wopr/zork/actors.c            wopr/zork/ballop.c        \
            wopr/zork/clockr.c            wopr/zork/demons.c        \
            wopr/zork/dgame.c             wopr/zork/dinit.c         \
	    wopr/zork_modified/dmain.c    wopr/zork/dso1.c          \
	    wopr/zork/dso2.c              wopr/zork_modified/dso3.c \
	    wopr/zork/dso4.c              wopr/zork/dso5.c          \
	    wopr/zork/dso6.c              wopr/zork/dso7.c          \
	    wopr/zork_modified/dsub.c     wopr/zork/dverb1.c        \
	    wopr/zork/dverb2.c            wopr/zork_modified/gdt.c  \
	    wopr/zork/lightp.c            wopr/zork/local.c         \
	    wopr/zork/nobjs.c             wopr/zork_modified/np.c   \
	    wopr/zork/np1.c               wopr/zork/np2.c           \
	    wopr/zork/np3.c               wopr/zork/nrooms.c        \
	    wopr/zork/objcts.c            wopr/zork/rooms.c         \
	    wopr/zork/sobjs.c             wopr/zork_modified/supp.c \
	    wopr/zork/sverbs.c            wopr/zork/verbs.c         \
	    wopr/zork/villns.c



SRCS_WOPR = wopr/wopr.cpp                     wopr/wopr_audio.cpp          \
            wopr/wopr_tictactoe.cpp           wopr/wopr_checkers.cpp       \
            wopr/wopr_chess.cpp               wopr/beatchess.cpp           \
            wopr/wopr_mines.cpp               wopr/wopr_maze.cpp           \
            wopr/wopr_war.cpp                 wopr/wopr_zork.cpp           \
            wopr/wopr_basic.cpp               wopr/chess_ai_move.cpp       \
            wopr/minesweeper_game.cpp         wopr/highscores.cpp          \
            wopr/wopr_basic/basic_print.cpp   wopr/wopr_basic/commands.cpp \
            wopr/wopr_basic/display_ansi.cpp  wopr/wopr_basic/expr.cpp     \
            wopr/wopr_basic/main.cpp          wopr/wopr_basic/program.cpp  \
            wopr/wopr_basic/sound_sdl.cpp     wopr/wopr_basic/vars.cpp     \
            wopr/wopr_willy.cpp               wopr/chess_sound.cpp         \
            wopr/wopr_sdi.cpp
                   



SRCS_LINUX = $(SRCS_COMMON) $(SRCS_WOPR) term_pty.cpp
SRCS_WIN   = $(SRCS_COMMON) $(SRCS_WOPR) term_pty_win.cpp

# ============================================================================
# OBJECTS
# ============================================================================
OBJECTS_LINUX       = $(addprefix $(BUILD_DIR_LINUX)/,       $(SRCS_LINUX:.cpp=.o)) \
                      $(addprefix $(BUILD_DIR_LINUX)/,        $(SRCS_MINIZ:.c=.o))
OBJECTS_LINUX_DEBUG = $(addprefix $(BUILD_DIR_LINUX_DEBUG)/,  $(SRCS_LINUX:.cpp=.debug.o)) \
                      $(addprefix $(BUILD_DIR_LINUX_DEBUG)/,  $(SRCS_MINIZ:.c=.debug.o))
OBJECTS_WIN         = $(addprefix $(BUILD_DIR_WIN)/,          $(SRCS_WIN:.cpp=.win.o)) \
                      $(addprefix $(BUILD_DIR_WIN)/,           $(SRCS_MINIZ:.c=.win.o))
OBJECTS_WIN_DEBUG   = $(addprefix $(BUILD_DIR_WIN_DEBUG)/,    $(SRCS_WIN:.cpp=.win.debug.o)) \
                      $(addprefix $(BUILD_DIR_WIN_DEBUG)/,    $(SRCS_MINIZ:.c=.win.debug.o))

EXECUTABLE_LINUX       = flt$(SSH_SUFFIX)
EXECUTABLE_LINUX_DEBUG = flt_debug$(SSH_SUFFIX)
ifeq ($(FELIXBASIC),1)
EXECUTABLE_WIN         = flt-felixbasic$(SSH_SUFFIX).exe
EXECUTABLE_WIN_DEBUG   = flt-felixbasic_debug$(SSH_SUFFIX).exe
else
EXECUTABLE_WIN         = flt$(SSH_SUFFIX).exe
EXECUTABLE_WIN_DEBUG   = flt_debug$(SSH_SUFFIX).exe
endif

BUILD_DIR             = build
BUILD_DIR_LINUX       = $(BUILD_DIR)/linux
BUILD_DIR_LINUX_DEBUG = $(BUILD_DIR)/linux_debug
BUILD_DIR_WIN         = $(BUILD_DIR)/windows
BUILD_DIR_WIN_DEBUG   = $(BUILD_DIR)/windows_debug

DLL_SOURCE_DIR = /usr/x86_64-w64-mingw32/sys-root/mingw/bin

$(shell mkdir -p \
    $(BUILD_DIR_LINUX) $(BUILD_DIR_LINUX_DEBUG) \
    $(BUILD_DIR_WIN)   $(BUILD_DIR_WIN_DEBUG) \
    $(BUILD_DIR_LINUX)/wopr/zork         $(BUILD_DIR_LINUX)/wopr/zork_modified \
    $(BUILD_DIR_LINUX_DEBUG)/wopr/zork   $(BUILD_DIR_LINUX_DEBUG)/wopr/zork_modified \
    $(BUILD_DIR_WIN)/wopr/zork           $(BUILD_DIR_WIN)/wopr/zork_modified \
    $(BUILD_DIR_WIN_DEBUG)/wopr/zork     $(BUILD_DIR_WIN_DEBUG)/wopr/zork_modified)

# ============================================================================
# TOP-LEVEL TARGETS
# ============================================================================
.PHONY: all linux windows debug clean clean-all help check-deps

all: linux

linux: flt-linux

windows: flt-windows flt-collect-dlls

debug: flt-linux-debug flt-windows-debug

# ============================================================================
# LINUX BUILD
# ============================================================================
.PHONY: flt-linux
flt-linux: index.hpp $(BUILD_DIR_LINUX)/$(EXECUTABLE_LINUX) frontend-linux

$(BUILD_DIR_LINUX)/$(EXECUTABLE_LINUX): index.hpp $(OBJECTS_LINUX)
	@echo "Linking Linux: $@"
	$(CXX_LINUX) $(CXXFLAGS_LINUX) $^ -o $@ $(LDFLAGS_LINUX)
	@echo "✓ $@"

$(BUILD_DIR_LINUX)/%.o: %.cpp
	@echo "Compiling (Linux): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX) -MMD -MP -c $< -o $@

# sound_sdl.cpp needs -DHAVE_SDL to enable the SDL audio backend.
# Without it the #ifdef HAVE_SDL guard makes the file compile to nothing
# and the silent sound_null stubs are used instead.
$(BUILD_DIR_LINUX)/wopr/wopr_basic/sound_sdl.o: wopr/wopr_basic/sound_sdl.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Linux): $< [+HAVE_SDL]"
	$(CXX_LINUX) $(CXXFLAGS_LINUX) -DHAVE_SDL -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX_DEBUG)/wopr/wopr_basic/sound_sdl.debug.o: wopr/wopr_basic/sound_sdl.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Linux Debug): $< [+HAVE_SDL]"
	$(CXX_LINUX) $(CXXFLAGS_LINUX_DEBUG) -DHAVE_SDL -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN)/wopr/wopr_basic/sound_sdl.win.o: wopr/wopr_basic/sound_sdl.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Windows): $< [+HAVE_SDL]"
	$(CXX_WIN) $(CXXFLAGS_WIN) -DHAVE_SDL -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN_DEBUG)/wopr/wopr_basic/sound_sdl.win.debug.o: wopr/wopr_basic/sound_sdl.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Windows Debug): $< [+HAVE_SDL]"
	$(CXX_WIN) $(CXXFLAGS_WIN_DEBUG) -DHAVE_SDL -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX)/wopr/%.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Linux): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux): $<"
	$(CC_LINUX) $(CFLAGS_LINUX) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX)/wopr/%.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux): $<"
	$(CC_LINUX) $(CFLAGS_LINUX) -MMD -MP -c $< -o $@

# ============================================================================
# LINUX DEBUG BUILD
# ============================================================================
.PHONY: flt-linux-debug
flt-linux-debug: index.hpp $(BUILD_DIR_LINUX_DEBUG)/$(EXECUTABLE_LINUX_DEBUG) frontend-linux

$(BUILD_DIR_LINUX_DEBUG)/$(EXECUTABLE_LINUX_DEBUG): index.hpp $(OBJECTS_LINUX_DEBUG)
	@echo "Linking Linux debug: $@"
	$(CXX_LINUX) $(CXXFLAGS_LINUX_DEBUG) $^ -o $@ $(LDFLAGS_LINUX_DEBUG)
	@echo "✓ $@"

$(BUILD_DIR_LINUX_DEBUG)/%.debug.o: %.cpp
	@echo "Compiling (Linux Debug): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX_DEBUG)/wopr/%.debug.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Linux Debug): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX_DEBUG)/%.debug.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux Debug): $<"
	$(CC_LINUX) $(CFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX_DEBUG)/wopr/%.debug.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux Debug): $<"
	$(CC_LINUX) $(CFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

# ============================================================================
# WINDOWS BUILD
# ============================================================================
# When FELIXBASIC=1, wipe all Windows object files first so every .cpp
# gets recompiled with -DFELIXBASIC_BUILD (and likewise when switching back).
.PHONY: flt-windows
ifeq ($(FELIXBASIC),1)
flt-windows: index.hpp clean-win-obj $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN) frontend-windows-dlls
else
flt-windows: index.hpp $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN) frontend-windows-dlls
endif

.PHONY: clean-win-obj
clean-win-obj:
	@echo "Cleaning Windows object files (FELIXBASIC build)..."
	find $(BUILD_DIR_WIN) -type f \( -name "*.win.o" -o -name "*.win.d" \) -delete 2>/dev/null || true

$(BUILD_DIR_WIN)/$(EXECUTABLE_WIN): index.hpp $(OBJECTS_WIN)
	@echo "Linking Windows: $@"
	$(CXX_WIN) $(CXXFLAGS_WIN) $^ -o $@ $(LDFLAGS_WIN)
	@echo "✓ $@"

$(BUILD_DIR_WIN)/%.win.o: %.cpp
	@echo "Compiling (Windows): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN)/wopr/%.win.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Windows): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN)/%.win.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows): $<"
	$(CC_WIN) $(CFLAGS_WIN) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN)/wopr/%.win.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows): $<"
	$(CC_WIN) $(CFLAGS_WIN) -MMD -MP -c $< -o $@

# ============================================================================
# WINDOWS DEBUG BUILD
# ============================================================================
.PHONY: flt-windows-debug
flt-windows-debug: index.hpp $(BUILD_DIR_WIN_DEBUG)/$(EXECUTABLE_WIN_DEBUG) frontend-windows-dlls

$(BUILD_DIR_WIN_DEBUG)/$(EXECUTABLE_WIN_DEBUG): index.hpp $(OBJECTS_WIN_DEBUG)
	@echo "Linking Windows debug: $@"
	$(CXX_WIN) $(CXXFLAGS_WIN_DEBUG) $^ -o $@ $(LDFLAGS_WIN)
	@echo "✓ $@"

$(BUILD_DIR_WIN_DEBUG)/%.win.debug.o: %.cpp
	@echo "Compiling (Windows Debug): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN_DEBUG)/wopr/%.win.debug.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Windows Debug): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN_DEBUG)/%.win.debug.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows Debug): $<"
	$(CC_WIN) $(CFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN_DEBUG)/wopr/%.win.debug.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows Debug): $<"
	$(CC_WIN) $(CFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

# ============================================================================
# FRONTEND BUILD TARGETS
# ============================================================================
.PHONY: frontend frontend-linux frontend-windows frontend-windows-dlls frontend-all frontend-clean

frontend: frontend-linux

frontend-linux: $(FRONTEND_LINUX)
	@echo "✓ FelixTerminalGUI (Linux) built successfully"
	@echo "   Output: $(FRONTEND_LINUX)"

$(FRONTEND_LINUX): frontend/FelixTerminalGUI.cpp
	@mkdir -p $(BUILD_DIR_LINUX)
	@echo "Compiling FelixTerminalGUI (Linux)..."
	$(CXX_LINUX) $(CXXFLAGS_FRONTEND_LINUX) $< -o $@ $(LDFLAGS_FRONTEND_LINUX)

frontend-windows: $(FRONTEND_WIN)
	@echo "✓ FelixTerminalGUI.exe (Windows) built successfully"
	@echo "   Output: $(FRONTEND_WIN)"

$(FRONTEND_WIN): frontend/FelixTerminalGUI.cpp
	@mkdir -p $(BUILD_DIR_WIN)
	@echo "Compiling FelixTerminalGUI (Windows)..."
	$(CXX_WIN) $(CXXFLAGS_FRONTEND_WIN) -mwindows $< -o $@ $(LDFLAGS_FRONTEND_WIN)

frontend-windows-dlls: $(FRONTEND_WIN)
	@echo "Collecting Windows DLLs for FelixTerminalGUI (64-bit)..."
	@if [ -f collect_dlls.sh ]; then \
		./collect_dlls.sh $(FRONTEND_WIN) /usr/x86_64-w64-mingw32/sys-root/mingw/bin $(BUILD_DIR_WIN); \
	else \
		echo "Error: collect_dlls.sh not found in root directory"; \
		exit 1; \
	fi
	@echo "✓ DLLs collected to $(BUILD_DIR_WIN)"

frontend-all: frontend-windows frontend-windows-dlls
	@echo "✓ Full Windows frontend build complete (binary + DLLs)"
	@echo "   Location: $(BUILD_DIR_WIN)/"

frontend-clean:
	@echo "Cleaning frontend binaries..."
	rm -f $(FRONTEND_LINUX) $(FRONTEND_WIN)

# ============================================================================
# DLL COLLECTION
# ============================================================================
.PHONY: flt-collect-dlls
flt-collect-dlls: $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN)
	@echo "Collecting DLLs..."
	@if [ -f collect_dlls.sh ]; then \
		./collect_dlls.sh $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN) $(DLL_SOURCE_DIR) $(BUILD_DIR_WIN); \
	else \
		echo "Tip: write collect_dlls.sh or copy manually:"; \
		echo "  SDL2.dll, glew32.dll, freetype.dll, libwinpthread-1.dll,"; \
		echo "  libgcc_s_seh-1.dll, libstdc++-6.dll,"; \
		echo "  libssh2.dll, libssl-*.dll, libcrypto-*.dll"; \
	fi

# ============================================================================
# GZIPPED WEB ASSETS
# ============================================================================
# Gzip index.html and convert to C header (saves ~100KB in binary)
# Automatically regenerated when index.hpptml changes
WEB_ASSETS = web/404.html web/index.html web/audioDetection.js web/audioVisualizer.js \
             web/cdgPlayer.js web/cookies.js web/fileFilter.js web/imageDetection.js \
             web/karaokePlayer.js web/karaoke-library.js web/panDragHandlers.js \
             web/slideshowAutoplay.js web/theme.js web/videoDetection.js web/videoPlayer.js \
             web/zipReader.js web/zoomPan.js web/crypto-js.min.js

ASSET_HEADERS = 404.hpp index.hpp audioDetection.hpp audioVisualizer.hpp cdgPlayer.hpp \
                cookies.hpp fileFilter.hpp imageDetection.hpp karaokePlayer.hpp \
                karaoke-library.hpp panDragHandlers.hpp slideshowAutoplay.hpp theme.hpp \
                videoDetection.hpp videoPlayer.hpp zipReader.hpp zoomPan.hpp crypto-js.hpp

index.hpp: $(WEB_ASSETS)
	@echo "Generating web asset headers..."
	@rm -f $(ASSET_HEADERS)
	@xxd -i -n 404_html web/404.html > 404.hpp
	@xxd -i -n index_html web/index.html > index.hpp
	@xxd -i -n audioDetection_js web/audioDetection.js > audioDetection.hpp
	@xxd -i -n audioVisualizer_js web/audioVisualizer.js > audioVisualizer.hpp
	@xxd -i -n cdgPlayer_js web/cdgPlayer.js > cdgPlayer.hpp
	@xxd -i -n cookies_js web/cookies.js > cookies.hpp
	@xxd -i -n fileFilter_js web/fileFilter.js > fileFilter.hpp
	@xxd -i -n imageDetection_js web/imageDetection.js > imageDetection.hpp
	@xxd -i -n karaokePlayer_js web/karaokePlayer.js > karaokePlayer.hpp
	@xxd -i -n karaoke_library_js web/karaoke-library.js > karaoke-library.hpp
	@xxd -i -n panDragHandlers_js web/panDragHandlers.js > panDragHandlers.hpp
	@xxd -i -n slideshowAutoplay_js web/slideshowAutoplay.js > slideshowAutoplay.hpp
	@xxd -i -n theme_js web/theme.js > theme.hpp
	@xxd -i -n videoDetection_js web/videoDetection.js > videoDetection.hpp
	@xxd -i -n videoPlayer_js web/videoPlayer.js > videoPlayer.hpp
	@xxd -i -n zipReader_js web/zipReader.js > zipReader.hpp
	@xxd -i -n zoomPan_js web/zoomPan.js > zoomPan.hpp
	@xxd -i -n crypto_js_min_js web/crypto-js.min.js > crypto-js.hpp
	@echo "✓ Converted web assets"


# ============================================================================
# DEPENDENCY FILES
# ============================================================================
-include $(OBJECTS_LINUX:.o=.d)
-include $(OBJECTS_LINUX_DEBUG:.o=.d)
-include $(OBJECTS_WIN:.o=.d)
-include $(OBJECTS_WIN_DEBUG:.o=.d)

# ============================================================================
# UTILITY
# ============================================================================
.PHONY: check-deps
check-deps:
	@echo "=== Linux ==="
	@$(PKG_CONFIG_LINUX) --exists freetype2 && echo "✓ freetype2" || echo "✗ freetype2"
	@$(PKG_CONFIG_LINUX) --exists sdl2      && echo "✓ sdl2"      || echo "✗ sdl2"
	@$(PKG_CONFIG_LINUX) --exists glew      && echo "✓ glew"      || echo "✗ glew"
	@$(PKG_CONFIG_LINUX) --exists gstreamer-1.0 && echo "✓ gstreamer-1.0" || echo "✗ gstreamer-1.0 (video playback)"
	@$(PKG_CONFIG_LINUX) --exists libssh2   && echo "✓ libssh2"   || echo "✗ libssh2 (required)"
	@$(PKG_CONFIG_LINUX) --exists libavformat libavcodec libavutil && echo "✓ ffmpeg" || echo "✗ ffmpeg (for audio conversion: libavformat, libavcodec, libavutil)"
	@echo "=== Windows (mingw64) ==="
	@$(PKG_CONFIG_WIN) --exists freetype2 && echo "✓ freetype2" || echo "✗ freetype2"
	@$(PKG_CONFIG_WIN) --exists sdl2      && echo "✓ sdl2"      || echo "✗ sdl2"
	@$(PKG_CONFIG_WIN) --exists glew      && echo "✓ glew"      || echo "✗ glew"
	@$(PKG_CONFIG_WIN) --exists gstreamer-1.0 && echo "✓ gstreamer-1.0" || echo "✗ gstreamer-1.0 (video playback)"
	@$(PKG_CONFIG_WIN) --exists libssh2   && echo "✓ libssh2"   || echo "✗ libssh2 (required)"

clean:
	find $(BUILD_DIR) -type f \( -name "*.o" -o -name "*.d" \) -delete 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX)/$(EXECUTABLE_LINUX)
	rm -f $(BUILD_DIR_LINUX_DEBUG)/$(EXECUTABLE_LINUX_DEBUG)
	rm -f $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN)
	rm -f $(BUILD_DIR_WIN_DEBUG)/$(EXECUTABLE_WIN_DEBUG)
	rm -f $(FRONTEND_LINUX) $(FRONTEND_WIN)
	rm -f $(ASSET_HEADERS)

clean-all:
	rm -rf $(BUILD_DIR)

help:
	@echo "Felix Terminal build targets:"
	@echo "  make              - Linux build (default)"
	@echo "  make linux        - Linux build"
	@echo "  make windows      - Windows cross-compile (mingw)"
	@echo "  make windows FELIXBASIC=1 - Windows build with -DFELIXBASIC_BUILD (felixbasic default shell)"
	@echo "  make debug        - Debug builds for both platforms"
	@echo "  make check-deps   - Verify all dependencies are present"
	@echo "  make clean        - Remove object files and binaries"
	@echo "  make clean-all    - Remove entire build/ directory"
	@echo ""
	@echo "Frontend (FelixTerminalGUI) targets:"
	@echo "  make frontend             - Build GUI frontend for Linux"
	@echo "  make frontend-linux       - Build GUI frontend for Linux (output: build/linux/)"
	@echo "  make frontend-windows     - Build GUI frontend for Windows (output: build/windows/)"
	@echo "  make frontend-windows-dlls - Build Windows binary + collect DLLs (output: build/windows/)"
	@echo "  make frontend-all         - Full Windows frontend build (binary + DLLs)"
	@echo "  make frontend-clean       - Remove frontend binaries"
	@echo ""
	@echo "Frontend requires:"
	@echo "  Linux:   wxWidgets development library (libwxgtk3.0-dev or wxGTK-devel)"
	@echo "  Windows: mingw64-wxWidgets or manually configured wxWidgets cross-build"
	@echo ""
	@echo "Windows cross-compile requires:"
	@echo "  x86_64-w64-mingw32-g++  (mingw-w64)"
	@echo "  mingw64-pkg-config"
	@echo "  mingw64 packages: sdl2, glew, freetype2"
	@echo "  On Fedora: sudo dnf install mingw64-SDL2 mingw64-glew mingw64-freetype"
	@echo "  On Ubuntu: sudo apt install mingw-w64 mingw-w64-tools"
	@echo "             (then build deps from source or use mxe.cc)"
	@echo ""
	@echo "SSH Linux deps:   sudo apt install libssh2-1-dev libssl-dev"
	@echo "                  sudo dnf install libssh2-devel openssl-devel"
	@echo "SSH Windows deps: mingw64-libssh2 mingw64-openssl (Fedora)"
	@echo "                  or build from source via mxe.cc"
	@echo ""
	@echo "Audio conversion (M4A/WMA/MP2) Linux deps:"
	@echo "  Ubuntu: sudo apt install libavformat-dev libavcodec-dev libavutil-dev"
	@echo "  Fedora: sudo dnf install ffmpeg-devel"
