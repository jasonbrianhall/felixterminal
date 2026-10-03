#pragma once
// The disk BASIC's files live on. Booted from a FAT12 floppy (drive A:),
// that's the floppy itself: LOAD, SAVE, OPEN, FILES, KILL... all work on it.
// Booted any other way (CD, USB stick, QEMU -kernel), it's a blank 1.44 MB
// RAM disk, so the same commands work but nothing outlives a reboot.
// Boot option floppy=off forces the RAM disk. A disk image the boot loader
// hands over (felixbasic.img next to felixbasic.efi) fills the RAM disk.
#include <stdint.h>
#include <stddef.h>

void storage_init(uint32_t mb_flags, uint32_t boot_device, const char* cmdline);
void storage_set_image(const void* image, uint32_t size);   // before storage_init
bool storage_on_floppy();                 // false: the RAM disk
bool storage_ready();                     // mounted (after a disk check on the floppy)
bool storage_write_protected();           // the last write failed on a protected disk
bool storage_disk_write_protected();      // the floppy in the drive is (checked now)

// Paths: '/' or '\' separators, optional "A:" in front, relative to the
// current directory, "." and ".." resolved. Out: an absolute path.
bool storage_resolve(const char* path, char* out, size_t cap);
const char* storage_cwd();
void storage_set_cwd(const char* abs_path);
