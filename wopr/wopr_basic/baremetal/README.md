# Bare-metal Felix BASIC

Boots straight into Felix BASIC, the way a home computer of the early '80s
booted into its BASIC: no operating system, just the `Ok` prompt. Programs
are typed, `RUN`, `LIST`ed, and `SAVE`d to and `LOAD`ed from the floppy the
machine booted from.

```
sudo apt install build-essential g++-multilib qemu-system-x86 grub-pc-bin grub-common mtools dosfstools xorriso
make floppy          # felixbasic-floppy.img: 1.44 MB boot floppy (64-bit)
make run-floppy      # boot it in QEMU
make ARCH=i386 floppy    # felixbasic-i386-floppy.img: for a 386 or later
make floppies        # both
make iso             # felixbasic.iso: CD / USB stick (files on a RAM disk)
make run             # QEMU, direct kernel boot (files on a RAM disk)
```

Write the floppy image to a real disk with `dd if=felixbasic-floppy.img of=/dev/fd0`.

## What's on the floppy

A FAT12 disk any PC can read: GRUB and the kernel in `/boot`, and the sample
programs from `../test` in the root. `FILES` lists them, `LOAD "gorilla"`
and `RUN` plays one. Everything the interpreter does with files works on the
same disk: `SAVE`, `LOAD`, `KILL`, `RENAME`, `MKDIR`, `CD`, `OPEN ... FOR
INPUT/OUTPUT/APPEND`. `A:` in front of a name is accepted and ignored.

Files are written whole when closed, new data first and the directory last,
so pulling the disk or losing power mid-save leaves the old file or the new
one, never half of one. A write-protected disk gives `Disk write-protected`;
a swapped disk is noticed and mounted.

Booted any other way (CD, USB stick, `make run`), or with `floppy=off`,
files go to a 1.44 MB RAM disk instead: the same commands work, but nothing
survives a reboot.

## The machine

| | `ARCH=x86_64` (default) | `ARCH=i386` |
|---|---|---|
| CPU | any 64-bit PC | 386 or later, **with a 387** (or a 486DX) |
| RAM | 6 MB (7 MB for 640x480 graphics) | 4 MB (5 MB for 640x480 graphics) |
| Screen | the monitor's own resolution, else 1024x768 or smaller | 640x480 at 8 bits (a 512 KB VESA card), else larger |
| Sound | HD Audio, AC97 or a Sound Blaster, else PC speaker | a Sound Blaster (Pro 2.0 or later), else PC speaker (or AC97/HD Audio on a PCI machine) |
| Keyboard | PS/2 and USB | PS/2 |
| Mouse | PS/2 and USB (with wheel) | PS/2 (with wheel) |
| Limits | 1024 variables, arrays up to 65536 elements | 512 variables, arrays up to 32768 elements |

Arrays take memory when they're `DIM`med, as much as they need, and so do
graphics pages when a `SCREEN` mode uses them; when there isn't enough, BASIC
says `Out of memory` and carries on (a mode that doesn't fit leaves the screen
in text mode).

BASIC numbers are doubles, so a 386 needs its math coprocessor; without one
the machine says so. The video card needs a VESA BIOS with a linear
framebuffer (VBE 2.0; UniVBE adds it to older cards).

The text screen is 80x25 (or 40x25 after `WIDTH 40`, with double-width
characters) in the 8x16 VGA font, scaled up to fill the screen (by whole
numbers when that comes close, so the font stays crisp). Graphics modes
(`SCREEN 1`, `7`, `9`, `12`, `13`, ...) fill the largest 4:3 area of the
screen, the shape of the monitors they were made for (as the SDL window
shows them), with the text grid over them; `CIRCLE` uses QBasic's aspect
ratio, so circles stay round. The CP437 box-drawing, block and shade
characters (`CHR$(176)`-`CHR$(223)`) are drawn too.

`SOUND`, `BEEP` and `PLAY` go through a sound card when there is one (HD
Audio, AC97, or a Sound Blaster Pro 2.0 or later at port 220h, 8-bit DMA
1), else the PC speaker, as on the original machines; they play from the
timer interrupt, so music keeps time however busy the program is.

Ctrl+C or Ctrl+Break stops a running program (`Break`); Shift+PgUp/PgDn
scroll back through the text; Ctrl+Alt+Del reboots. `SYSTEM` asks whether
to reboot.

The mouse (PS/2, or USB on the 64-bit build) works as in the SDL window.
At the prompt, drag with the left button to select text, Ctrl+C copies it
(instead of breaking) and Ctrl+V types it back, line breaks as Enter; the
wheel scrolls back through the text. Programs get it too, the QuickBASIC
way (the DOS mouse driver, INT 33h, through `CALL INTERRUPT(&H33, inregs,
outregs)` or the usual `CALL ABSOLUTE` mouse routine) or the QB64 way
(`_MOUSEINPUT`, `_MOUSEX`, `_MOUSEY`, `_MOUSEBUTTON(n)`, `_MOUSEWHEEL`,
`_MOUSESHOW`, `_MOUSEHIDE`, `_MOUSEMOVE`); while one does, the mouse is
the program's. The pointer is a reversed character cell in text modes and
an arrow in graphics modes, as the DOS driver drew them; at the prompt it
shows while the mouse is in use. `MOUSE.BAS` and `MOUSE33.BAS` on the
floppy try both ways.

Boot options (on the `multiboot` line in `/boot/grub/grub.cfg`):
`audio=speaker|hda|ac97|sb|off`, `sb=220,1` (the Sound Blaster's port and
8-bit DMA channel, as in `BLASTER=A220 D1`), `floppy=off`, `latency=MS`
(sound card buffering).

## How it's put together

The interpreter (`../main.cpp`, `vars.cpp`, `expr.cpp`, `program.cpp`,
`commands.cpp`, `basic_print.cpp`) is compiled as for the SDL window build
(`USE_SDL_WINDOW`, graphics on) without GMP. Around it:

- `basic_gfx_bm.cpp` is generated from `../basic_gfx_sdl.cpp` by
  `tools/make_gfx_bm.py`: the palette, pixel pages, drawing, sprites, text
  grid, scrollback and line editor are the SDL backend's own code; the
  window, FreeType and event loop are replaced with the framebuffer, the
  8x16 font and the keyboard. The Makefile regenerates it whenever the SDL
  backend changes.
- `sound_bm.cpp` (the SDL build's PLAY parser) and `sound_player.cpp` (the
  note player, run from the timer interrupt).
- `libc.cpp`: printf with real floating-point formatting, strtod, math on
  the x87, stdio/dirent/stat over FAT12, time from the CMOS clock;
  `setjmp.S`, `runtime.cpp` (memory, strings, heap), `stdhooks.cpp`,
  `rbtree.cpp` (std::map without libstdc++'s prebuilt objects).
- `storage.cpp` (floppy or RAM disk, current directory, paths),
  `fat12.cpp`, `floppy.cpp`.
- `kernel.cpp`: boot, memory, video, keyboard, PS/2 mouse, timer; `boot.S`/`boot32.S`,
  `irq.cpp`, `audio.cpp`, `pci.cpp`, `usb.cpp`.

Tests that run on a Linux host: `tools/fat12_test.sh` (the filesystem,
checked with fsck.fat and mtools), `tools/libc_test.sh` (printf, strtod
and math against glibc), `tools/rbtree_test.sh`.

The serial port (COM1) logs boot progress; `make run` shows it.
