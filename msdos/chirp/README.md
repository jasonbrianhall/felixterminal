Felix Chirp for MS-DOS
======================

Felix Chirp, Felix Terminal's F5 viewer, as a DOS program of its own:
`CHIRP.EXE`, a 32-bit DPMI program made with DJGPP. It shows pictures,
plays music, CD+G and .kfn karaoke, shows text and Markdown, and looks
inside zip files, with a file browser down the left. It runs on vmdos (or
any DOS with VESA and a DPMI host; CWSDPMI is built in for plain DOS).

    C:\> CHIRP                       (the current folder)
    C:\> CHIRP D:\MUSIC              (another one)
    C:\> CHIRP C:\PICS\CAT.JPG       (open a file at once)
    C:\> CHIRP -g 1280x800 D:\       (another screen mode)

What it reads
-------------

* Pictures: JPEG, PNG, BMP, GIF, WebP (stb_image, libwebp). Zoom with the
  wheel, drag to pan, R rotates, 0 resets, L runs a slideshow.
* Music: MP3, OGG Vorbis, FLAC, WAV, AIFF, VOC, MIDI (played on
  felixterminal's own OPL3 FM synth), and tracker modules: MOD, XM, IT,
  S3M, 669, MED, MTM and the rest libxmp knows. Opus isn't in the DOS build.
* Karaoke: a .cdg next to its .mp3 (or in the same zip), and .kfn files.
* Text: .txt, .md (Markdown: headers, lists, code), and the like.
* Zip files: browse inside them, and open what's there.

Video (GStreamer on Linux) isn't in the DOS build, nor is the remote (SFTP)
browsing Felix Terminal does over SSH.

Drives
------

The root of a drive (C:\) lists the other drives as folders (`[DIR] D:`):
pick one to go there. Or press **Alt+letter** (Alt+D) from anywhere.
`..` at a drive's root stays there.

Keys
----

    Up/Down, PgUp/PgDn, Home/End   move in the list
    Enter, double-click            open
    Left/Right                     previous / next file
    Space                          pause / resume
    S, Backspace                   stop
    Shift+Left/Right, Shift+Up/Down  -5s/+5s, -30s/+30s in a song
    + / -                          volume
    V                              next visualizer
    .                              repeat: off, one, all
    K / B                          .kfn: vocal / backing track on and off
    a letter                       the next file starting with it
    double-click the picture       fullscreen
    Esc                            back (fullscreen, a text file), then quit
    F10, Ctrl+Q                    quit

Options
-------

    CHIRP [-g WxH[xBITS]] [-f size] [-l] [folder | file]

* `-g` (or `SET CHIRP_VIDEO=WxH`): the VESA mode. The default is 1024x768,
  32-bit if there is one, else 16-bit; otherwise the largest mode of
  640x480 or more.
* `-f`: the font size in pixels (default 16).
* `-l`: list the VESA modes.
* `SET CHIRP_LOG=file`: a debug log (the sound card, frame times).

Sound needs a Sound Blaster (vmdos's: Pro or 16) with `BLASTER` set, as
vmdos's AUTOEXEC.BAT does. Without one, pictures and text still work.
Songs in zips and .kfn tracks are unpacked to `%TEMP%` (else `C:\TEMP`),
and deleted after.

How it's built
--------------

It is felixterminal's `felixchirp/`, compiled as it is (with
`#ifdef __DJGPP__` in a few places: no GStreamer, and the drive list),
with what it takes from the rest of Felix Terminal supplied here:

* `chirp_dos.cpp`: the front end. A VESA mode through SDL3's DOS port
  (`sdl3/`), SDL's software renderer; `draw_rect` and `draw_text`
  (FreeType, the embedded DejaVu Sans Mono and Noto Emoji); the text
  viewer's `glBegin`/`glEnd`; `mkstemps`/`mkstemp` for `%TEMP%`. DOS's
  threads are cooperative, so the main loop waits a little after every
  frame: that is when SDL's audio thread mixes.
* `mixer_dos.cpp`: the part of SDL2_mixer's API Chirp uses, on SDL3's
  audio, with dr_mp3, dr_flac, dr_wav, stb_vorbis, libxmp and
  felixterminal's `midi_render.cpp` (OPL3) and `aiff_render.cpp`.
* `aes_dos.c`: AES decryption for encrypted .kfn files (OpenSSL's
  `AES_decrypt` interface), so no OpenSSL.
* `sdl3/`, `cwsdpmi/`: SDL3 with its DOS port, and CWSDPMI's stub,
  copied from SDLPoPDOS unchanged (`sdl3/README.chirp`).
* `include/`: SDL2 names on SDL3 (`SDL2/SDL.h`, `SDL2/SDL_mixer.h`), and
  stand-ins for OpenGL, libssh2 and `openssl/aes.h`.
* `chirp_compat.h`: forced into every file; FLT's `dos_compat.h` and what
  DJGPP's library lacks (`mkstemps`, `fminf`, `fmaxf`).

Building
--------

    cd msdos/chirp
    make            # the libraries the first time, then CHIRP.EXE
    make dist       # build/dist/CHIRP: CHIRP.EXE and README.TXT

It needs make and git, and either:

* a DJGPP cross compiler, found as FLT's is (`../build-djgpp.sh` builds one
  into `~/djgpp`; or on the PATH, or `DJGPP=dir`), with git; or
* Docker, used when there is no compiler (`DOCKER=1` forces it, `DOCKER=0`
  forbids it), with FLT's image (`../Dockerfile`, built as
  `felixterminal-djgpp`).

`make deps` is `build-deps.sh`: it
builds, into `deps/`, FreeType 2.13.3, libwebp 1.5.0 (the decoder) and
libxmp 4.6.3, and fetches dr_libs and stb at pinned commits. SDL3 with its
DOS port is in `sdl3/` (exact copies of SDLPoPDOS's, with CWSDPMI's stub in
`cwsdpmi/`: `sdl3/README.chirp`), and `make` compiles it. With
`upx` there, the EXE is packed (6 MB to 3 MB). Unlike FLT there's no
OpenSSL, libssh2 or vmdos sockets, so no perl or host gcc is needed.
