**Bare-metal Felix BASIC**

Boots straight into Felix BASIC the way an early-’80s home computer did: no operating system, just the `Ok` prompt. Type programs, `RUN` them, `LIST` them, `SAVE` them, and `LOAD` them from the same floppy the machine started from.

```
sudo apt install build-essential g++-multilib qemu-system-x86 grub-pc-bin grub-common mtools dosfstools xorriso
make floppy            # felixbasic-floppy.img — 1.44 MB boot floppy (64-bit)
make run-floppy        # boot it in QEMU
make ARCH=i386 floppy  # felixbasic-i386-floppy.img — for 386 or later
make floppies          # both images
make iso               # felixbasic.iso — CD/USB (files live on a RAM disk)
make run               # QEMU, direct kernel boot (files on a RAM disk)
make efi floppy        # builds felixbasic.efi and the matching image
```

Write a floppy image to a real disk with:

```
dd if=felixbasic-floppy.img of=/dev/fd0
```

### Booting on real hardware from Linux

Build the EFI version and image:

```
make efi floppy
```

Then copy the two files into the EFI system partition:

```
sudo mkdir -p /boot/efi/EFI/felixbasic
sudo cp felixbasic.efi felixbasic.img /boot/efi/EFI/felixbasic/
```

Add a menu entry by editing `/etc/grub.d/40_custom` (or creating it if it does not exist):

```
#!/usr/bin/sh
exec tail -n +3 $0
# This file provides an easy way to add custom menu entries.  Simply type the
# menu entries you want to add after this comment.  Be careful not to change
# the 'exec tail' line above.
#
menuentry "Felix Basic OS" {
    insmod part_gpt
    insmod fat
    insmod chain
    search --no-floppy --file --set=root /EFI/felixbasic/felixbasic.efi
    chainloader /EFI/felixbasic/felixbasic.efi debug
}
```

Update GRUB and reboot:

```
sudo update-grub
```

The new entry appears in the GRUB menu and boots straight into Felix BASIC.

### What’s on the floppy

A normal FAT12 disk that any PC can read. GRUB and the kernel sit in `/boot`; the sample programs from `../test` are in the root. Type `FILES` to see them, then `LOAD "gorilla"` and `RUN` to play one.

All the usual file commands work on the same disk: `SAVE`, `LOAD`, `KILL`, `RENAME`, `MKDIR`, `CD`, and `OPEN … FOR INPUT/OUTPUT/APPEND`. Putting `A:` in front of a name is fine; it is simply ignored.

Files are written completely when they are closed (data first, directory last), so if the power fails or you pull the disk you never end up with a half-written file. A write-protected disk just says `Disk write-protected`. If you swap disks the change is noticed and the new one is mounted.

When the machine is started any other way (CD, USB stick, or `make run`) or with `floppy=off`, everything goes to a 1.44 MB RAM disk instead. The same commands work, but nothing survives a reboot.

### Hardware

|          | 64-bit build (default)                                      | 32-bit build (`ARCH=i386`)                                              |
|----------|-------------------------------------------------------------|-------------------------------------------------------------------------|
| CPU      | any 64-bit PC                                               | 386 or later with a math chip (387 or 486DX)                            |
| RAM      | 6 MB (7 MB if you want 640×480 graphics)                    | 4 MB (5 MB for 640×480)                                                 |
| Screen   | the monitor’s native resolution, otherwise 1024×768 or less | 640×480 at 8-bit colour (needs a 512 KB VESA card), or larger           |
| Sound    | HD Audio, AC97 or Sound Blaster; otherwise the PC speaker   | Sound Blaster preferred; otherwise PC speaker (or AC97/HD Audio on PCI) |
| Keyboard | PS/2 or USB                                                 | PS/2 or USB                                                             |
| Mouse    | PS/2 or USB (with wheel)                                    | PS/2 or USB (with wheel)                                                |
| Limits   | 1024 variables, arrays up to 65 536 elements                | 512 variables, arrays up to 32 768 elements                             |

Arrays and graphics pages only take the memory they actually need. If there is not enough left, BASIC simply says `Out of memory` and carries on. A graphics mode that will not fit stays in text mode.

Numbers are ordinary doubles, so a plain 386 needs its floating-point chip. The video card needs a VESA BIOS that offers a linear framebuffer (VBE 2.0; UniVBE adds it to older cards).

### Screen, sound and input

Text is 80×25 (or 40×25 after `WIDTH 40`) in the classic 8×16 VGA font, scaled up so it fills the screen cleanly. Graphics modes (`SCREEN 1`, `7`, `9`, `12`, `13` \ldots) use the largest 4:3 rectangle that fits—the shape the old monitors had—so circles drawn with `CIRCLE` stay round. All the usual CP437 box and block characters (`CHR$(176)`–`CHR$(223)`) are drawn.

`SOUND`, `BEEP` and `PLAY` go through a real sound card when one is present (HD Audio, AC97 or a Sound Blaster Pro 2.0 or later at port 220h / DMA 1). Otherwise they use the PC speaker. Music is driven by the timer interrupt, so it keeps time no matter how busy the program is.

- Ctrl+C or Ctrl+Break stops a running program.
- Shift+PgUp / PgDn scrolls the text buffer.
- Ctrl+Alt+Del reboots; `SYSTEM` asks first.

The mouse works the same way it does in the SDL window. At the prompt you can drag to select text, Ctrl+C copies it and Ctrl+V pastes it back. Programs receive mouse input either the old QuickBASIC way (`CALL INTERRUPT(&H33, \ldots)` or `CALL ABSOLUTE`) or the QB64 way (`_MOUSEINPUT`, `_MOUSEX`, `_MOUSEY`, etc.). While a program is using the mouse it owns the pointer. The samples `MOUSE.BAS` and `MOUSE33.BAS` on the floppy show both methods.

### Boot options

These can be added to the `multiboot` line in `/boot/grub/grub.cfg`:

- `audio=speaker|hda|hdmi|analog|ac97|sb|off`
- `hda=BB:DD.F` — pick a particular HD Audio controller
- `sb=220,1` — Sound Blaster port and DMA channel
- `floppy=off`
- `latency=MS` — sound-card buffering
- `pause` — hold the start-up messages until a key is pressed
- `usb=off`

Ctrl+F1 at any time opens a menu of every audio output found at boot so you can switch on the fly. `DMESG` at the BASIC prompt shows the boot messages again.

### How it is built

The interpreter itself is the same code that runs in the SDL window (`main.cpp`, `vars.cpp`, `expr.cpp`, `program.cpp`, `commands.cpp`, `basic_print.cpp`), compiled without GMP.

Around it:

- `basic_gfx_bm.cpp` is generated from the SDL graphics code by `tools/make_gfx_bm.py`. Palette, pages, drawing, sprites, text grid, scrollback and line editor are shared; the window, FreeType and event loop are replaced by the framebuffer, the 8×16 font and the keyboard.
- `sound_bm.cpp` and `sound_player.cpp` handle `PLAY` and the interrupt-driven note player.
- `libc.cpp` supplies floating-point `printf`, `strtod`, x87 maths, FAT12 file I/O and CMOS time, plus a small C++ runtime.
- `storage.cpp`, `fat12.cpp` and `floppy.cpp` manage the disk or the RAM disk.
- `kernel.cpp` and its helpers look after boot, memory, video, keyboard, mouse, timer, PCI, USB and audio.

Host-side tests live in `tools/fat12_test.sh`, `tools/libc_test.sh` and `tools/rbtree_test.sh`.

Boot progress is logged on the serial port (COM1); `make run` shows it.

# Limitations

The software currently has several limitations

- Their are some QBasic bugs.  It's not a perfect interpreter.  Bugs are fixed as they are found.
- USB keyboards and mice work on any xHCI (USB 3) controller: the motherboard's, an add-in card, or several at once (up to 8), including through hubs.  Also no hot plugging; if you unplug it and need to use it, you'll have to reboot.
- No HDMI Audio support.  Will never happen.  Their are just too many driver variations for such a small project.
- Limited to FAT12.  FAT12 is old.  It's good enough for a filesystem for loading and saving basic files but that's about it.
- Secure boot will never work.  Not paying Microsoft to sign my efi files.  It does boot with security boot disabled and in legacy boot mode.
- Can't boot from the floppy in EFI mode.  This is just because EFI doesn't have floppy support.  Either boot from a CDROM or boot from Linux using an existing GRUB install.


