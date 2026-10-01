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
| Screen | 640x480, else larger | 640x480 at 8 bits (a 512 KB VESA card), else larger |
| Sound | HD Audio or AC97, else PC speaker | PC speaker (or AC97/HD Audio on a PCI machine) |
| Keyboard | PS/2 and USB | PS/2 |
| Limits | 1024 variables, arrays up to 65536 elements | 512 variables, arrays up to 32768 elements |

Arrays take memory when they're `DIM`med, as much as they need, and so do
graphics pages when a `SCREEN` mode uses them; when there isn't enough, BASIC
says `Out of memory` and carries on (a mode that doesn't fit leaves the screen
in text mode).

BASIC numbers are doubles, so a 386 needs its math coprocessor; without one
the machine says so. The video card needs a VESA BIOS with a linear
framebuffer (VBE 2.0; UniVBE adds it to older cards).

The text screen is 80x25 (or 40x25 after `WIDTH 40`, with double-width
characters) in the 8x16 VGA font, scaled up by whole numbers on bigger
screens. Graphics modes (`SCREEN 1`, `7`, `9`, `12`, `13`, ...) are drawn at
whole-number scales close to the shape of a 4:3 monitor, with the text grid
over them as in the SDL build. The CP437 box-drawing, block and shade
characters (`CHR$(176)`-`CHR$(223)`) are drawn too.

`SOUND`, `BEEP` and `PLAY` go through the PC speaker, as on the original
machines, or a PCI sound card when there is one; they play from the timer
interrupt, so music keeps time however busy the program is.

Ctrl+C or Ctrl+Break stops a running program (`Break`); Shift+PgUp/PgDn
scroll back through the text; Ctrl+Alt+Del reboots. `SYSTEM` asks whether
to reboot.

Boot options (on the `multiboot` line in `/boot/grub/grub.cfg`):
`audio=speaker|hda|ac97|off`, `floppy=off`, `latency=MS` (sound card
buffering).

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
- `kernel.cpp`: boot, memory, video, keyboard, timer; `boot.S`/`boot32.S`,
  `irq.cpp`, `audio.cpp`, `pci.cpp`, `usb.cpp`.

Tests that run on a Linux host: `tools/fat12_test.sh` (the filesystem,
checked with fsck.fat and mtools), `tools/libc_test.sh` (printf, strtod
and math against glibc), `tools/rbtree_test.sh`.

The serial port (COM1) logs boot progress; `make run` shows it.
