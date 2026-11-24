# Builds only the Linux Version; can theoretically be used for Windows but couldn't get it working in Visual Studio

cmake_minimum_required(VERSION 3.24)
project(felixterminal C CXX)

# Forces every target created by find_package() to be globally visible
# rather than directory-scoped. Needed here because some vcpkg port configs
# (e.g. SDL2_mixer's, which transitively depends on zlib/libpng) resolve
# their own dependencies via nested find_package() calls in a scope that,
# without this, doesn't propagate the resulting imported target (e.g.
# ZLIB::ZLIB) back up to this top-level CMakeLists.txt -- even though the
# "found" cache variables leak through fine, leaving downstream
# target_link_libraries() calls unable to see the target at all.
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

option(FLT_SSH "Build with libssh2 SSH/SFTP support" ON)
option(FLT_FELIXBASIC "Build with -DFELIXBASIC_BUILD" OFF)

# ============================================================================
# DEPENDENCIES
#   Windows: vcpkg dynamic triplet (e.g. x64-windows), CONFIG mode.
#   Linux:   native dev packages via pkg-config, same modules the Makefile
#            already probes for (wx-config for wxWidgets, as in ClearText).
# ============================================================================
set(FLT_CODEC_LIBS)   # accumulated optional codec link targets/flags
set(FLT_CODEC_FOUND)  # accumulated compile defs for codecs actually found

if(WIN32)
    find_package(ZLIB REQUIRED)

    find_package(SDL2 CONFIG REQUIRED)
    find_package(SDL2_mixer CONFIG REQUIRED)
    find_package(GLEW REQUIRED)
    find_package(Freetype REQUIRED)
    find_package(PNG REQUIRED)
    find_package(WebP CONFIG REQUIRED)

    # Codec libs optional, same as the pkg-config fallbacks in the Makefile --
    # if a port isn't installed, its feature is left out rather than failing
    # the build. sound_sdl.cpp etc. guard on their own #ifdef checks.
    find_package(unofficial-libxmp CONFIG QUIET)
    find_package(MPG123 CONFIG QUIET)
    find_package(Opus CONFIG QUIET)
    find_package(OpusFile CONFIG QUIET)
    find_package(Vorbis CONFIG QUIET)
    find_package(FLAC CONFIG QUIET)

    if(unofficial-libxmp_FOUND)
        list(APPEND FLT_CODEC_LIBS unofficial::libxmp::libxmp)
    endif()
    if(MPG123_FOUND)
        list(APPEND FLT_CODEC_LIBS MPG123::libmpg123)
    endif()
    if(Opus_FOUND)
        list(APPEND FLT_CODEC_LIBS Opus::opus)
    endif()
    if(OpusFile_FOUND)
        list(APPEND FLT_CODEC_LIBS OpusFile::opusfile)
    endif()
    if(Vorbis_FOUND)
        list(APPEND FLT_CODEC_LIBS Vorbis::vorbisfile Vorbis::vorbis)
    endif()
    if(FLAC_FOUND)
        list(APPEND FLT_CODEC_LIBS FLAC::FLAC)
    endif()

    if(FLT_SSH)
        find_package(Libssh2 CONFIG REQUIRED)
        find_package(OpenSSL REQUIRED)
    endif()

    # wxWidgets for the frontend, vcpkg CONFIG mode.
    find_package(wxWidgets CONFIG REQUIRED COMPONENTS core base)

    # MSVC has no native pthread.h; beatchess.h reaches for one.
    find_package(PThreads4W REQUIRED)

    # GStreamer is deliberately NOT searched for on Windows. Source guards on
    # #ifdef HAVE_GSTREAMER (left undefined here), so gstreamer code compiles
    # out and the Windows exe stays gstreamer/plugin-DLL free.
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(GLEW REQUIRED glew)
    pkg_check_modules(FREETYPE REQUIRED freetype2)
    pkg_check_modules(GSTREAMER QUIET gstreamer-1.0 gstreamer-app-1.0)
    pkg_check_modules(FFMPEG QUIET libavformat libavcodec libavutil libswresample)

    pkg_check_modules(LIBXMP QUIET libxmp)
    pkg_check_modules(LIBMPG123 QUIET libmpg123)
    pkg_check_modules(OPUSFILE QUIET opusfile)
    pkg_check_modules(OPUS QUIET opus)
    pkg_check_modules(VORBISFILE QUIET vorbisfile)
    pkg_check_modules(FLACPC QUIET flac)

    foreach(codec IN ITEMS LIBXMP LIBMPG123 OPUSFILE OPUS VORBISFILE FLACPC)
        if(${codec}_FOUND)
            list(APPEND FLT_CODEC_LIBS ${${codec}_LIBRARIES})
        endif()
    endforeach()

    if(FLT_SSH)
        pkg_check_modules(LIBSSH2 REQUIRED libssh2)
    endif()

    # wxWidgets: use CMake's bundled FindwxWidgets module (shells out to
    # wx-config), same as the ClearText Linux build -- not CONFIG mode,
    # which only works with vcpkg's exported config files.
    find_package(wxWidgets REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
endif()

# ============================================================================
# SOURCES (mirrors SRCS_COMMON / SRCS_WOPR / SRCS_MINIZ in the Makefile)
# ============================================================================
set(SRCS_COMMON
    felixterminal.cpp gl_renderer.cpp sdl_renderer.cpp ft_font.cpp
    term_color.cpp terminal.cpp term_ui.cpp gl_bouncingcircle.cpp
    fight_mode.cpp crt_audio.cpp felix_settings.cpp kitty_graphics.cpp
    font_manager.cpp sftp_overlay.cpp sftp_console.cpp
    felixchirp/felixchirp.cpp felixchirp/cdg.cpp
    telnet_session.cpp basic_graphics.cpp mml_player.cpp
    glyph_atlas.cpp sticky_prompt.cpp ssh_key_manager.cpp serial_session.cpp
    felixchirp/fc_file.cpp felixchirp/fc_text.cpp felixchirp/fc_input.cpp
    felixchirp/fc_zip.cpp felixchirp/fc_kfn.cpp felixchirp/kfn.cpp
    sftp_webserver.cpp midi_render.cpp dbopl.cpp dbopl_wrapper.cpp
    instruments.cpp voc_render.cpp au_render.cpp aiff_render.cpp
    convertm4atowavlin.cpp convertm4atowavwin.cpp
    vfs.cpp sixel_graphics.cpp term_clipboard.cpp
)

if(WIN32)
    list(APPEND SRCS_COMMON term_pty_win.cpp)
else()
    list(APPEND SRCS_COMMON term_pty.cpp)
endif()

if(FLT_SSH)
    list(APPEND SRCS_COMMON ssh_session.cpp port_forward.cpp pf_overlay.cpp)
endif()

set(SRCS_WOPR
    wopr/wopr.cpp wopr/wopr_audio.cpp wopr/wopr_tictactoe.cpp
    wopr/wopr_checkers.cpp wopr/wopr_chess.cpp wopr/beatchess.cpp
    wopr/wopr_mines.cpp wopr/wopr_maze.cpp wopr/wopr_war.cpp
    wopr/wopr_zork.cpp wopr/wopr_basic.cpp wopr/chess_ai_move.cpp
    wopr/minesweeper_game.cpp wopr/highscores.cpp
    wopr/wopr_basic/basic_print.cpp wopr/wopr_basic/commands.cpp
    wopr/wopr_basic/display_ansi.cpp wopr/wopr_basic/expr.cpp
    wopr/wopr_basic/main.cpp wopr/wopr_basic/program.cpp
    wopr/wopr_basic/sound_sdl.cpp wopr/wopr_basic/vars.cpp
    wopr/wopr_willy.cpp wopr/chess_sound.cpp wopr/wopr_sdi.cpp
    wopr/wopr_speech.cpp
)

set(SRCS_MINIZ
    libtelnet.c felixchirp/miniz.c felixchirp/miniz_tdef.c
    felixchirp/miniz_tinfl.c felixchirp/miniz_zip.c
    wopr/zork/actors.c wopr/zork/ballop.c wopr/zork/clockr.c
    wopr/zork/demons.c wopr/zork/dgame.c wopr/zork/dinit.c
    wopr/zork_modified/dmain.c wopr/zork/dso1.c wopr/zork/dso2.c
    wopr/zork_modified/dso3.c wopr/zork/dso4.c wopr/zork/dso5.c
    wopr/zork/dso6.c wopr/zork/dso7.c wopr/zork_modified/dsub.c
    wopr/zork/dverb1.c wopr/zork/dverb2.c wopr/zork_modified/gdt.c
    wopr/zork/lightp.c wopr/zork/local.c wopr/zork/nobjs.c
    wopr/zork_modified/np.c wopr/zork/np1.c wopr/zork/np2.c
    wopr/zork/np3.c wopr/zork/nrooms.c wopr/zork/objcts.c
    wopr/zork/rooms.c wopr/zork/sobjs.c wopr/zork_modified/supp.c
    wopr/zork/sverbs.c wopr/zork/verbs.c wopr/zork/villns.c
    wopr/synth/synth.c
)

add_executable(flt WIN32 ${SRCS_COMMON} ${SRCS_WOPR} ${SRCS_MINIZ})

if(MSVC)
    target_compile_options(flt PRIVATE /MP /Zc:strictStrings-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # This codebase's C-style BASIC/Zork interpreter APIs take char* (not
    # const char*) for read-only string params throughout basic.h, so
    # passing string literals trips -Wwrite-strings everywhere. Same
    # tradeoff as /Zc:strictStrings- above -- suppress the warning class
    # rather than widen dozens of signatures blind.
    target_compile_options(flt PRIVATE -Wno-write-strings)
endif()

target_compile_definitions(flt PRIVATE WOPR DONTUSEGMP)
if(WIN32)
    target_compile_definitions(flt PRIVATE WIN32 _WIN32 _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN __STDC__=1 _CRT_DECLARE_NONSTDC_NAMES=1)
else()
    target_compile_definitions(flt PRIVATE LINUX)
endif()
if(FLT_SSH)
    target_compile_definitions(flt PRIVATE USESSH)
endif()
if(FLT_FELIXBASIC)
    target_compile_definitions(flt PRIVATE FELIXBASIC_BUILD)
endif()
if(NOT WIN32 AND GSTREAMER_FOUND)
    target_compile_definitions(flt PRIVATE HAVE_GSTREAMER)
endif()

# sound_sdl.cpp needs -DHAVE_SDL to enable the SDL audio backend (same as
# the Makefile's dedicated rule for this one file).
set_source_files_properties(wopr/wopr_basic/sound_sdl.cpp PROPERTIES
    COMPILE_DEFINITIONS HAVE_SDL
)

target_include_directories(flt PRIVATE ${CMAKE_SOURCE_DIR} wopr)

if(WIN32)
    target_link_libraries(flt PRIVATE
        SDL2::SDL2main SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        GLEW::GLEW
        Freetype::Freetype
        PNG::PNG
        ZLIB::ZLIB
        WebP::webp
        PThreads4W::PThreads4W
        opengl32 winmm shlwapi ws2_32
        mf mfplat mfreadwrite mfuuid ole32 oleaut32
        ${FLT_CODEC_LIBS}
    )
    if(FLT_SSH)
        target_link_libraries(flt PRIVATE Libssh2::libssh2 OpenSSL::SSL OpenSSL::Crypto ws2_32)
    endif()
else()
    target_include_directories(flt PRIVATE
        ${SDL2_INCLUDE_DIRS} ${GLEW_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS}
    )
    target_link_libraries(flt PRIVATE
        ${SDL2_LIBRARIES} ${GLEW_LIBRARIES} ${FREETYPE_LIBRARIES}
        GL png z m pthread stdc++ SDL2_mixer webp
        ${FLT_CODEC_LIBS}
    )
    if(GSTREAMER_FOUND)
        target_include_directories(flt PRIVATE ${GSTREAMER_INCLUDE_DIRS})
        target_link_libraries(flt PRIVATE ${GSTREAMER_LIBRARIES})
    endif()
    if(FFMPEG_FOUND)
        target_include_directories(flt PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(flt PRIVATE ${FFMPEG_LIBRARIES})
    endif()
    if(FLT_SSH)
        target_include_directories(flt PRIVATE ${LIBSSH2_INCLUDE_DIRS})
        target_link_libraries(flt PRIVATE ${LIBSSH2_LIBRARIES} ssl crypto)
    endif()
endif()

# ============================================================================
# FRONTEND (FelixTerminalGUI) -- single-file wx launcher/config GUI
# ============================================================================
add_executable(FelixTerminalGUI WIN32 frontend/FelixTerminalGUI.cpp)
if(WIN32)
    target_compile_definitions(FelixTerminalGUI PRIVATE WIN32 _WIN32 _WIN32_WINNT=0x0A00)
    target_link_libraries(FelixTerminalGUI PRIVATE ${wxWidgets_LIBRARIES})
    target_include_directories(FelixTerminalGUI PRIVATE ${wxWidgets_INCLUDE_DIRS})
else()
    target_link_libraries(FelixTerminalGUI PRIVATE ${wxWidgets_LIBRARIES})
endif()
