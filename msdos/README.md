Felix Terminal for MS-DOS
=========================

Felix Terminal's terminal, its sixel and kitty graphics and its built-in SSH
client, as a DOS program: `FLT.EXE`, a 32-bit DPMI program made with DJGPP.
It runs under [vmdos](https://github.com/jasonbrianhall/vmdos), whose own
TCP/IP it uses, so you can SSH from DOS to a Linux box and run `timg`,
`img2sixel`, gnuplot's sixel terminal and the like, with the pictures on
the DOS screen.

    C:\> STARTNET                      (once: the network, by DHCP)
    C:\> FLT user@host                 (or FLT host, FLT user@host:2222)
    C:\> FLT -g 1280x800 user@host     (another screen mode)

It's the same code as on Linux and Windows: `terminal.cpp`, `term_color.cpp`,
`sixel_graphics.cpp`, `kitty_graphics.cpp` and `ssh_session.cpp` (libssh2),
with `#ifdef __DJGPP__` in a few places of `ssh_session.cpp` (vmdos's
sockets; no X11 forwarding). What's DOS-specific is in this folder:

* `flt_dos.cpp`, the front end in place of `felixterminal.cpp`,
  `term_ui.cpp`, `ft_font.cpp` and the GL renderer. The screen is a VESA
  mode through SDL3's DOS port (in `sdl3/`), fullscreen. The text is
  drawn with FreeType and the embedded DejaVu Sans Mono (bold and oblique
  too, FreeMono for symbols it lacks) into a buffer in memory, only the rows
  the terminal marks dirty, and block elements (half blocks, quadrants) as
  exact rectangles. Sixel and kitty images are drawn over it by
  `sixel_render()`/`kitty_render()` on their existing SDL path, through
  SDL's software renderer. Keys are turned into what `handle_key()` sends.
  `ssh_connect()` runs on the one thread there is; its host key and
  password questions are asked in the terminal.
* `include/GL/glew.h`: stand-ins for the OpenGL names in the sixel and kitty
  files. Their OpenGL code never runs on DOS (`g_use_sdl_renderer` is true).
* `include/SDL2/SDL.h`: the SDL2 calls those files make, on SDL3.
* `dos_compat.h`: mutexes that never wait (DOS runs one thread; DJGPP's
  libstdc++ has no `std::mutex`).

Not in the DOS build: the OpenGL effects, sound, Felix BASIC graphics, WOPR,
Felix Chirp (a DOS program of its own: `chirp/`, CHIRP.EXE), SFTP, the web server, port forwarding, the SSH key manager,
telnet, serial, a local shell, menus and the clipboard.

Options
-------

    FLT [-4|-6] [-p port] [-l user] [-i keyfile] [-g WxH[xBITS]] [-f size]
        [-t theme] [user@]host[:port] [command]

* `-g` (or `SET FLT_VIDEO=WxH`): the VESA mode. The default is 1024x768,
  32-bit if there is one, else 16-bit; otherwise the largest mode of
  640x480 or more.
* `-f`: the font size in pixels (16: 10x22 cells, 102x34 characters at
  1024x768).
* `-i`: the private key. Otherwise `~/.ssh/config`'s IdentityFile, then
  the usual `~/.ssh/id_ed25519`, `id_rsa`, ... `~` is `%HOME%`, `C:\HOME` if
  not set (as vmdos's SSH); `known_hosts` is there too.
* `-4` / `-6`: IPv4 or IPv6 only (as `ssh -4` / `-6`). Otherwise the
  resolver's order: IPv6 first only if this PC has a global address, so a
  host with both is reached over IPv4 when vmdos has just a ULA.
* `-t` (or `SET FLT_THEME=`): a theme, by name (or its start) or number.
* Shift+PgUp/PgDn/Home/End scroll back (images too); any other key goes
  back.

Mouse (a DOS mouse driver, e.g. CTMOUSE, or vmdos's):

* Left-drag selects and copies on release; middle-click or Shift+Insert
  pastes (FLT's own clipboard; bracketed paste if the program asked).
* When a program asks for the mouse (vim `set mouse=a`, htop, mc), clicks
  go to it; hold Shift to select instead.
* Right-click: a menu with Copy, Paste, Themes and Resolution. Resolution
  lists the VESA modes and switches at once (the PTY is resized). Esc
  closes it.

The theme and mode chosen are kept in `%HOME%\FLT.CFG` (`theme=`, `mode=WxHxBITS`);
`-g`/`FLT_VIDEO` and `-t`/`FLT_THEME` override it. `SET FLT_LOG=file` writes
a debug log.

TERM is `xterm-256color`. `timg -pk` draws with kitty graphics and
`timg -ps` with sixel; timg reads the cell size from the pixel size FLT
gives the PTY.

Building
--------

    cd msdos
    make            # the libraries the first time (a while: OpenSSL), then FLT.EXE
    make dist       # build/dist/FLT: FLT.EXE and README.TXT

It needs a checkout of [vmdos](https://github.com/jasonbrianhall/vmdos) next
to this one (`../../vmdos` from here, or `VMDOS=dir`), and either:

* a DJGPP cross compiler: `sh build-djgpp.sh` builds the one FLT.EXE is
  made and tested with (gcc 14.2.0, binutils 2.42, DJGPP's library from
  jwt27's djgpp-cvs, with [build-gcc](https://github.com/jwt27/build-gcc)
  at pinned commits; about an hour) into `~/djgpp`, where `make` finds it
  (as in `/opt/djgpp` and `/usr/local/djgpp`; or `DJGPP=dir`, or on the
  PATH); plus git, perl, python3 and a host gcc with `-m32` (gcc-multilib), or
* Docker, used when there is no DJGPP compiler (`DOCKER=1` forces it,
  `DOCKER=0` forbids it): the `Dockerfile` here adds git, perl, python3, a
  host gcc and upx to the `djfdyuruiry/djgpp` image SDLPoPDOS builds with,
  as `felixterminal-djgpp`, and the build runs in it with this tree and
  vmdos mounted.

`make deps` (which `make` runs the first time) is `build-deps.sh`: it builds,
into `deps/`, at pinned versions: zlib, libpng,
FreeType, OpenSSL (vmdos's `tools/build-openssl.sh`), libssh2 1.11.1
(`libssh2-dos.py`: non-blocking sockets by vmdos's `ioctlsocket()`;
`libssh2_config.h`) and vmdos's sockets (`third_party/openssh/dos-sock.c`, `third_party/openssl/ossl-dos.c`:
TCP and DNS through vmdos, `send`/`recv`/`select`, `getentropy`, the clock).
Watt-32's headers are used for the socket declarations; none of its code is
linked. With `upx` there, the EXE is packed (8 MB to 4 MB). `make distclean`
removes the libraries too.

SDL3 with its DOS port (VESA, keyboard, mouse) is checked in, in `sdl3/`:
exact copies of the files SDLPoPDOS's DOS build compiles, taken from
[SDLPoPDOS](https://github.com/jasonbrianhall/SDLPoPDOS) (`sdl3/README.sdl3`
says which commit and how to update them). `make` compiles them into
`build/libSDL3.a` (`sdl3/sdl3.mk`, which CHIRP's build uses too); no
SDLPoPDOS checkout is needed.
