# Felix Terminal

[![Microsoft Store](https://get.microsoft.com/images/en-us%20dark.svg)](https://get.microsoft.com/installer/download/9ns1cjk9vt7q?referrer=appbadge)

### Install

**Windows (winget):**
```
winget install "Felix Terminal"
```
This installs directly from the Microsoft Store listing above via winget's `msstore` source, so it stays up to date the same way Store installs do.

**Windows (manual):** download the `.msi` from the [Releases](../../releases) page. Same code as the Store/winget version, but installed this way it won't auto-update — you'll need to grab new releases manually. *(The `.msix` in Releases is unsigned and won't install directly outside the Store — use the MSI instead for manual installs.)*

**Linux:** build from source — see [Building](#building) below.

> Felix Terminal was previously called **GL Terminal**. Some source files (e.g. `gl_terminal.h`) and the repository name still carry the old name.


![Felix the Lovebird](icon.png)

*Felix — the chaos birdie himself*

## The Story Behind the Name

Felix Terminal is named after Felix, a lovebird whose life was tragically cut short. Felix had a particular fondness for perching on top of computer monitors and supervising whatever was happening on screen — and occasionally causing as much mischief as a small bird possibly could. In his honor, this terminal carries his name. Every time it opens, a little chaos birdie lives on.

Rest easy, Felix. 🐦

---

Felix Terminal is a standalone OpenGL terminal emulator for Linux and MS Windows with SDL fallback for rendering (some shaders in SDL are CPU rendered so they are slow). Renders text using FreeType triangles via OpenGL 3.3 — no GTK, no Qt, no desktop toolkit dependency.

## Features

- VT100/VT220/xterm-compatible emulation
- Full 256-color and 24-bit RGB color support
- Font variants: Regular, Bold, Italic, Bold Italic (DejaVu Sans Mono)
- Text attributes: bold, italic, underline, strikethrough, overline, dim, blink, reverse
- Emoji rendering via embedded NotoEmoji (fallback for non-ASCII glyphs)
- Scrollback buffer (5000 lines)
- Mouse reporting (X10 and SGR encoding)
- Bracketed paste mode
- Alternate screen buffer (used by vim, less, etc.)
- Configurable themes and window opacity
- Spawn additional terminal windows
- Works with /bin/bash, cmd.exe, powershell, and many more
- Inline graphics: **Sixel** and **Kitty** graphics protocols, including animated GIFs — see [Inline Graphics](#inline-graphics)
- **Copy as Rich Text** — paste terminal output into Word or LibreOffice with colors, formatting and images intact
- URL detection with Ctrl+Click to open in browser
- System font selection from any installed monospace font
- Includes Felix BASIC
- Built-in **SSH**, **Telnet** (with optional SSL/TLS) and **serial/RS-232** connections — see [Usage](#usage)
- **F1** help screen listing every shortcut
- **F7** WOPR Terminal — a hidden retro mainframe with chess, checkers, Zork, Wizard's Castle, minesweeper, tic-tac-toe and more

## Inline Graphics

Felix Terminal displays images inline using both the **Sixel** and **Kitty** graphics protocols, including animated GIFs. Works with tools such as `timg`, `img2sixel` and gnuplot. Images scroll with the text, stay in the scrollback, and are removed by `clear` along with the text around them. *(Kitty graphics on Windows is untested.)*

**Copy as Rich Text** (right-click menu, or `Ctrl+C` with a selection) puts the selection on the clipboard as HTML with colors, bold/italic and inline images embedded, so it pastes into Word, LibreOffice Writer or an email with graphics intact. **Copy as HTML** and **Copy as ANSI** are also available.

### gnuplot tip

gnuplot's sixel terminal defaults to `anchor` mode, which moves the cursor to the top-left of the screen before drawing, so the plot lands on top of existing text. Use `scroll` to draw it at the cursor instead:

```
gnuplot -p -e "set term sixelgd truecolor scroll; plot sin(x)"
```

To make this the default, add `set term sixelgd truecolor scroll` to `~/.gnuplot`.

## SSH Support

- Built-in SSH client via **libssh2** — no external SSH binary required
- Authentication: SSH agent (including **Pageant** on Windows), public key file, and password
- **CAC/PIV smart card support** via Pageant — works out of the box with standard DoD/government PKCS#11 middleware (e.g. OpenSC feeding keys into Pageant)
- Host key verification against `~/.ssh/known_hosts` on Linux and Windows (`%USERPROFILE%\.ssh\known_hosts`), with an OpenSSH-style prompt for new hosts (answer `yes` or paste the fingerprint) and a loud warning if a known host's key changes. New keys are appended; the file is never rewritten, so `@revoked`/`@cert-authority` lines are preserved
- Ed25519, ECDSA, and RSA host key types supported
- Keepalive to prevent server-side idle disconnect
- PTY resize forwarded to remote on window resize
- Password prompt rendered natively inside the terminal window (no external dialog)
- X11 forwarding (disable with `--no-x11`)
- Launch with `--ssh [user@host[:port]]` or via the **New Terminal → SSH Session** context menu item

## SFTP File Transfer

- Integrated graphical SFTP browser — no separate client needed, old-school dual-pane "Norton Commander"-style file copy screen (no drag-and-drop — you navigate each pane and select what to transfer)
- **F2** — Upload: left panel browses local files, right panel browses remote destination
- **F3** — Download: right panel browses remote files, left panel selects local destination
- **F4** — SFTP Console (interactive command-line SFTP; type `help` for commands)
- **F5** — Felix Chirp (view remote and local images and listen to audio, including Karaoke CD+G files)
- **F6** — Port forwarding console: add local (`L`), remote (`R`) and SOCKS5 dynamic (`D`) forwards
- **F8** — SSH Key Manager: generate, copy and delete local keys, and manage the remote `authorized_keys`
- Tab switches focus between panels; Enter opens directories; Backspace navigates up; Space transfers
- Real-time progress bar with bytes transferred / total during upload and download
- Transfers run on a background thread — terminal remains responsive during large file transfers
- Remote working directory auto-detected via `pwd` on session open
- Downloads saved to user-chosen local directory (defaults to `~/Downloads/FelixTerminal`)
- SFTP subsystem shares the existing SSH session — no second connection or re-authentication

F2, F3, F4, F6 and the remote half of F8 require a session opened with the built-in SSH client (`--ssh` or **New Terminal → SSH Session**); they don't apply to an `ssh` command typed into a local shell.

### Felix Stargate Web File Browser (F9)

- **F9** — toggles a web file browser at `http://localhost:53716`. In a built-in SSH session it serves the **remote** filesystem over SFTP; otherwise it serves **local** files. Press F9 again to stop it.
- Can also be started from the command line with `--webserver [address:port]` (and `--web-root <dir>` for local mode)
- Port `53716` spells **FELIX**: F→5, E→3, L→7 (upside-down L), I→1, X→6
- Binds to `127.0.0.1` by default — reachable from the local machine only, so there's nothing to expose or firewall
- Works on IPv6 and IPv4-only systems: it uses a dual-stack socket when IPv6 is available and falls back to IPv4 when it isn't
- If port `53716` is already in use, it automatically tries the next port up (to `53815`) — check the debug log (**F12**) for the actual port if it had to fall back
- **Read-only**: browse directories, sort by name/type/size/modified, and download files straight from a browser tab — handy for quick access without opening the F2/F3 panels. Nothing can be uploaded, changed or deleted through it; use F2/F3 for transfers
- Each browser request runs on its own thread against its own SFTP subsystem, so a large transfer through the web browser won't block the terminal or the F4 console
- "Open in new window" checkbox in the browser UI controls whether clicking a file opens a new tab or navigates the current one — persisted as a cookie
- Shuts down automatically when the SSH session ends

## GL Render Modes

Post-process effects applied after terminal rendering — multiple modes can be active simultaneously:

| Mode | Description |
|---|---|
| Normal | Standard rendering |
| CRT | Scanline flicker and phosphor glow |
| LCD | Subpixel grid overlay |
| VHS | Noise, chroma bleed, and tracking artifacts |
| Focus | Vignette darkening outside the active row |
| C64 | Commodore 64 palette and chunky pixel look |
| Bad Composite | NTSC composite color bleeding |
| Bloom | Glow around bright text |
| Ghosting | Phosphor persistence trails |
| Wireframe | Grid overlay |

## Dependencies

Required:

- SDL2 and SDL2_mixer
- OpenGL 3.3 / GLEW
- FreeType2
- GStreamer 1.0 (+ plugins-base, for video playback in Felix Chirp)
- FFmpeg libraries (libavformat, libavcodec, libavutil, libswresample)
- libpng, libjpeg, libwebp, libtiff, giflib, zlib
- OpenSSL
- GMP
- wxWidgets 3.x (for the `FelixTerminalGUI` launcher)
- libssh2 (SSH, SFTP, port forwarding, key manager and remote web browser)
- `xxd` (used at build time to embed the web UI; usually in the `vim-common` or `xxd` package)

Optional:

- libxmp, mpg123, opus/opusfile, libvorbis, FLAC — extra audio formats in Felix Chirp (auto-detected)

On Debian/Ubuntu (tested on Ubuntu 24.04):
```
sudo apt install build-essential pkg-config xxd \
    libsdl2-dev libsdl2-mixer-dev libglew-dev libfreetype-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
    libpng-dev libjpeg-dev libwebp-dev libtiff-dev libgif-dev zlib1g-dev \
    libssl-dev libgmp-dev libwxgtk3.2-dev \
    libssh2-1-dev
# optional audio codecs
sudo apt install libxmp-dev libmpg123-dev libopus-dev libopusfile-dev libvorbis-dev libflac-dev
```

On Fedora/RHEL:
```
sudo dnf install gcc-c++ make pkgconf xxd \
    SDL2-devel SDL2_mixer-devel glew-devel freetype-devel \
    gstreamer1-devel gstreamer1-plugins-base-devel \
    ffmpeg-free-devel \
    libpng-devel libjpeg-turbo-devel libwebp-devel libtiff-devel giflib-devel zlib-devel \
    openssl-devel gmp-devel wxGTK-devel \
    libssh2-devel
# optional audio codecs
sudo dnf install libxmp-devel mpg123-devel opus-devel opusfile-devel libvorbis-devel flac-devel
```

Run `make check-deps` to see which libraries were found.

## Building

Linux:
```
make
```

Windows (cross-compiled with mingw64):
```
make windows
```

Output goes to `build/linux/` (or `build/windows/`):

| File | Description |
|---|---|
| `flt` / `flt.exe` | Felix Terminal |
| `FelixTerminalGUI` | wxWidgets launcher for picking connection options |

Debug builds: `make debug` (output in `build/linux_debug/`).

The fonts are embedded as base64-encoded headers — no external font files required.

## Usage

```
./build/linux/flt [command]
```

Optionally pass a command to run instead of the default shell:

```
./build/linux/flt htop
```

SSH session (opens connection dialog if host/user not specified):

```
flt --ssh
flt --ssh user@host
flt --ssh user@host:2222
flt --ssh-key ~/.ssh/id_ed25519 --ssh user@host
flt --ssh user@host -L 8080:localhost:80      # with a local port forward
flt --ssh user@host -c /bin/sh                # use a specific remote shell
flt --ssh user@host -c "tmux new -A -s main"  # attach to (or start) a persistent tmux session
```

Telnet:

```
flt --telnet host[:port]
flt --telnet host --ssl                       # Telnet over SSL/TLS
```

Serial / RS-232 console (prompts for port and settings if omitted):

```
flt --serial /dev/ttyUSB0 --serial-baud 115200
flt --serial COM3 --serial-baud 9600          # Windows
```

Run `flt --help` for the full list of options.

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `F1` | Help screen |
| `Ctrl+C` (with selection) | Copy selection |
| `Ctrl+Shift+C` (with selection) | Copy selection as HTML |
| `Ctrl+V` | Paste |
| `Ctrl+Shift+V` | Paste |
| `Ctrl+Scroll Up/Down` | Increase / decrease font size |
| `Ctrl+Shift+Scroll` | Increase / decrease font size (4× step) |
| `Ctrl+Click` | Open URL in browser |
| `Shift+PageUp / Shift+PageDown` | Scroll scrollback buffer |
| `F2` | SFTP upload browser *(built-in SSH sessions only)* |
| `F3` | SFTP download browser *(built-in SSH sessions only)* |
| `F4` | SFTP console *(built-in SSH sessions only)* |
| `F5` | Felix Chirp image / audio / karaoke viewer |
| `F6` | Port forwarding console *(built-in SSH sessions only)* |
| `F7` | WOPR Terminal |
| `F8` | SSH Key Manager |
| `F9` | Toggle web file browser at `localhost:53716` |
| `F11` | Toggle full screen |
| `F12` | Debug log |


## Mouse

| Action | Behaviour |
|---|---|
| Left click + drag | Select text |
| Release after drag | Auto-copy selection to clipboard |
| Middle click | Paste clipboard |
| Right click | Open context menu |
| Scroll wheel | Scroll scrollback history |
| `Ctrl` + scroll | Resize font |

## Context Menu (Right Click)

- **New Terminal** — spawn a new local terminal window
- **New Terminal → SSH Session** — open a new SSH session window
- **Duplicate Session** — open another window connected the same way (same host, user, port, key and remote shell for SSH; same host for Telnet; same shell and working directory for a local terminal). Port forwards and the web server aren't duplicated, and SSH asks for the password again unless a key or agent is used. Serial sessions can't be duplicated, since the port is already in use.
- **Copy** — copy selection as plain text
- **Copy as HTML** — copy with color and style markup
- **Copy as ANSI** — copy with ANSI escape codes
- **Paste**
- **Reset** — clear screen and reset cursor
- **Themes** — submenu to switch color themes
- **Opacity** — submenu to set window transparency
- **Render Mode** — submenu to toggle GL post-process effects
- **Fonts** — submenu to select from installed system monospace fonts
- **Sound** — enables or disables CRT audio effect
- **Fight Mode** — have two guys fight in your console
- **Bouncing Circle** — a bouncing ball overlay
- **Select All**
- **Quit**

## SSH Command-Line Flags

All `--ssh-*` flags also accept a single-dash form (e.g. `-ssh-key`).

| Flag | Description |
|:---|:---|
| `--ssh [user@host[:port]]` | Connect via SSH. Host and user are prompted inside the window if omitted. Port defaults to 22. IPv6 addresses go in brackets: `user@[::1]:22`. `User`, `Port` and `IdentityFile` from `~/.ssh/config` are applied for the host when not given on the command line. |
| `-i <path>` | Private key file (same as `--ssh-key`, matches standard `ssh` convention). |
| `--ssh-key <path>` | Private key file for public key authentication. |
| `--ssh-key-pub <path>` | Public key file. Derived from `--ssh-key` path (appending `.pub`) if omitted. |
| `--ssh-password <pass>` | Password. Not recommended — prefer agent or key auth. |
| `--ssh-known-hosts <path>` | Known hosts file. Default: `~/.ssh/known_hosts` (`%USERPROFILE%\.ssh\known_hosts` on Windows — shared with Windows' built-in `ssh`). Host keys are always verified; there is no option to skip the check. |
| `--no-x11` | Disable X11 forwarding. |
| `-c <command>` | Run a remote shell or command instead of the login shell (alias: `--ssh-command`). It runs with a terminal attached, so `-c /bin/sh` or `-c "tmux new -A -s main"` is fully interactive. In the launcher this is the **Remote shell** field on the SSH tab. |
| `-L local_port:remote_host:remote_port` | Local port forward. |
| `-R remote_port:local_host:local_port` | Remote port forward. |
| `-D local_port` | SOCKS5 dynamic port forward. |

## Themes

| Theme | Description |
|---|---|
| Default | Dark blue-black background |
| Solarized Dark | Ethan Schoonover's Solarized palette |
| Monokai | Classic Monokai |
| Nord | Arctic, north-bluish palette |
| Gruvbox | Retro groove palette |
| Matrix | Black background, green text |
| Ocean | Deep blue background |

## Configuration

Runtime configuration is stored in:

- **Linux:** `~/.config/FelixTerminal/`
- **Windows:** Registry

Compile-time defaults are defined at the top of `gl_terminal.h`:

```c
#define TERM_COLS_DEFAULT  80
#define TERM_ROWS_DEFAULT  24
#define SCROLLBACK_LINES   5000
#define FONT_SIZE_DEFAULT  16
#define FONT_SIZE_MIN      6
#define FONT_SIZE_MAX      72
```

## Embedded Fonts

| File | Font |
|---|---|
| `DejaVuMono.h` | DejaVu Sans Mono Regular |
| `DejaVuMonoBold.h` | DejaVu Sans Mono Bold |
| `DejaVuMonoOblique.h` | DejaVu Sans Mono Oblique |
| `DejaVuMonoBoldOblique.h` | DejaVu Sans Mono Bold Oblique |
| `NotoEmoji.h` | Noto Emoji (grayscale fallback) |

To regenerate a font header, base64-encode the TTF and wrap it with the expected macro name and size constant (see any existing `.h` for the format).

## Troubleshooting

Felix Terminal needs OpenGL 3.3. On older hardware or a Raspberry Pi (Mesa/VC4), if it fails to start, try:

```
MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 flt
```

Add both variables to `~/.profile` to make this permanent.
