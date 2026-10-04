// The disk BASIC's files live on. See storage.hpp.
#include "storage.hpp"
#include "fat12.hpp"
#include "floppy.hpp"
#include "hw.hpp"

extern "C" {
int printf(const char*, ...);
void* memset(void*, int, size_t);
void* memcpy(void*, const void*, size_t);
size_t strlen(const char*);
int strncmp(const char*, const char*, size_t);
void* malloc(size_t);
long time(long*);
}

static bool on_floppy, mounted;
static char cwd[256] = "/";

// ---------------------------------------------------------------- RAM disk
static uint8_t* ram;                      // 2880 sectors, formatted FAT12 like a 1.44 MB floppy
static bool ram_read(uint32_t lba, uint32_t n, void* buf) {
    if (lba + n > 2880) return false;
    memcpy(buf, ram + lba * 512, n * 512);
    return true;
}
static bool ram_write(uint32_t lba, uint32_t n, const void* buf) {
    if (lba + n > 2880) return false;
    memcpy(ram + lba * 512, buf, n * 512);
    return true;
}
static const uint8_t* image;              // a disk image from the boot loader
static uint32_t image_size;
void storage_set_image(const void* p, uint32_t size) { image = (const uint8_t*)p; image_size = size; }

static bool ram_format() {
    ram = (uint8_t*)malloc(2880 * 512);
    if (!ram) return false;
    memset(ram, 0, 2880 * 512);
    if (image && image_size >= 512 && image[510] == 0x55 && image[511] == 0xAA) {
        memcpy(ram, image, image_size > 2880 * 512 ? 2880 * 512 : image_size);
        return true;
    }
    uint8_t* b = ram;
    static const uint8_t bpb[] = {
        0xEB, 0x3C, 0x90, 'F', 'E', 'L', 'I', 'X', ' ', ' ', ' ',
        0x00, 0x02,             // 512 bytes per sector
        0x01,                   // 1 sector per cluster
        0x01, 0x00,             // 1 reserved sector
        0x02,                   // 2 FATs
        0xE0, 0x00,             // 224 root entries
        0x40, 0x0B,             // 2880 sectors
        0xF0,                   // media: 3.5" 1.44 MB
        0x09, 0x00,             // 9 sectors per FAT
        0x12, 0x00, 0x02, 0x00, // 18 per track, 2 heads
    };
    memcpy(b, bpb, sizeof bpb);
    b[510] = 0x55; b[511] = 0xAA;
    for (int f = 0; f < 2; f++) {         // FAT entries 0 and 1
        uint8_t* fat = ram + (1 + f * 9) * 512;
        fat[0] = 0xF0; fat[1] = 0xFF; fat[2] = 0xFF;
    }
    return true;
}

// ---------------------------------------------------------------- floppy
static void idle() { floppy_idle(); }
static const FatDisk floppy_disk = {floppy_read, floppy_write, idle};
static const FatDisk ram_disk = {ram_read, ram_write, nullptr};

bool storage_on_floppy() { return on_floppy; }
bool storage_write_protected() { return on_floppy && floppy_write_protected(); }
bool storage_disk_write_protected() { return on_floppy && floppy_disk_write_protected(); }

// The floppy can be swapped while BASIC runs: check before each use, and
// remount when a different disk is in (back to the root directory).
bool storage_ready() {
    if (!on_floppy) return mounted;
    int m = floppy_check_media();
    if (m < 0) {
        if (mounted) printf("Storage: floppy removed\n");
        mounted = false;
    } else if (m == 1 || !mounted) {
        mounted = fat_mount(floppy_disk);
        cwd[0] = '/'; cwd[1] = 0;
        if (mounted) printf("Storage: FAT12 floppy mounted, %u KB free\n", fat_free_bytes() / 1024);
        else printf("Storage: the floppy isn't FAT12\n");
    }
    if (!mounted) floppy_idle();
    return mounted;
}

void storage_init(uint32_t mb_flags, uint32_t boot_device, const char* cmdline) {
    bool off = false;
    for (const char* p = cmdline; p && *p; p++)
        if (!strncmp(p, "floppy=off", 10)) off = true;
        else if (!strncmp(p, "floppy=trace", 12)) floppy_set_trace(true);
    // Multiboot boot_device: BIOS drive number in the top byte; 0x00 is A:.
    if (!off && (mb_flags & (1 << 1)) && (boot_device >> 24) == 0x00 && floppy_init()) {
        on_floppy = true;
        if (storage_ready()) return;
        on_floppy = false;
    }
    if (ram_format() && fat_mount(ram_disk)) {
        mounted = true;
        printf(image ? "Storage: disk image loaded into a 1.44 MB RAM disk\n"
                     : "Storage: no boot floppy; files go to a 1.44 MB RAM disk\n");
    } else {
        printf("Storage: none\n");
    }
}

// ---------------------------------------------------------------- paths
const char* storage_cwd() { return cwd; }
void storage_set_cwd(const char* p) {
    size_t n = strlen(p);
    if (n >= sizeof cwd) return;
    memcpy(cwd, p, n + 1);
}

bool storage_resolve(const char* path, char* out, size_t cap) {
    if (!path || cap < 2) return false;
    if (((path[0] | 32) == 'a') && path[1] == ':') path += 2;    // A:\FOO.BAS
    char buf[512];
    size_t n = 0;
    bool abs = path[0] == '/' || path[0] == '\\';
    if (!abs) {
        size_t c = strlen(cwd);
        if (c >= sizeof buf) return false;
        memcpy(buf, cwd, c);
        n = c;
        buf[n++] = '/';
    }
    for (const char* p = path; *p && n < sizeof buf - 1; p++) buf[n++] = *p == '\\' ? '/' : *p;
    buf[n] = 0;
    // Normalize into out: split on '/', drop "." and empty parts, ".." pops.
    size_t o = 0;
    out[o++] = '/';
    for (char* p = buf; *p;) {
        while (*p == '/') p++;
        char* e = p;
        while (*e && *e != '/') e++;
        size_t len = e - p;
        if (len == 0) break;
        if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; }   // back to the previous '/'
            if (o > 1) o--;
        } else {
            if (o > 1) { if (o + 1 >= cap) return false; out[o++] = '/'; }
            if (o + len >= cap) return false;
            memcpy(out + o, p, len);
            o += len;
        }
        p = e;
    }
    out[o] = 0;
    return true;
}

// File dates from the clock (libc.cpp: CMOS time, kept local).
long fat_clock() { return time(nullptr); }
