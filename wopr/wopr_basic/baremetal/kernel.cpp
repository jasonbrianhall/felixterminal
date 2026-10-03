// Bare-metal Felix BASIC: boots straight to the BASIC prompt, like a home
// computer of the early '80s. The interpreter (../main.cpp, expr.cpp, ...)
// is compiled unchanged; basic_gfx_bm.cpp draws its screen, sound_bm.cpp
// plays SOUND/PLAY/BEEP, and libc.cpp + storage.cpp put its files (LOAD,
// SAVE, OPEN, FILES...) on the boot floppy. This file is the machine: boot
// information, memory, the framebuffer, the keyboard, mouse, timer and restart.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdarg.h>
#include "hw.hpp"
#include "video.hpp"
#include "audio.hpp"
#include "pci.hpp"
#include "usb.hpp"
#include "floppy.hpp"
#include "storage.hpp"
#include "sound_player.hpp"
#include "font.h"
#include "basic_gfx_sdl.h"
#include "display.h"
#include <SDL2/SDL.h>

int main(int argc, char** argv);                       // ../main.cpp: the interpreter
#include "basic.h"                                     // sizeof(Var), for the memory check

// ---------------------------------------------------------------- serial
#define COM1 0x3F8
extern "C" void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (uint8_t)c);
}
extern "C" void serial_puts(const char* s) { while (*s) serial_putc(*s++); }
static void serial_init() {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7);
}

// ---------------------------------------------------------------- multiboot
struct __attribute__((packed)) MultibootInfo {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr, drives_length, drives_addr;
    uint32_t config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height;
    uint8_t fb_bpp, fb_type;
    uint8_t fb_pad[2];      // GRUB aligns what follows to 4 bytes
    uint8_t fb_color[6];    // type 1 (RGB): red, green, blue (position, size) pairs
};
struct __attribute__((packed)) MultibootMmap { uint32_t size; uint64_t addr, len; uint32_t type; };
extern "C" uint32_t mb_magic, mb_info;
extern "C" char __kernel_start[], __kernel_end[];
extern "C" void heap_add(void* p, size_t n);
extern "C" size_t heap_free_bytes(void), heap_peak_bytes(void);
extern "C" uint64_t phys_limit;
uint64_t phys_limit = 0x100000000ull;

// ---------------------------------------------------------------- memory
// Give the heap every usable RAM region the boot loader reports, minus the
// kernel image and anything below 1 MiB. Uses the Multiboot memory map
// (GRUB, QEMU, and the UEFI loader, which translates the UEFI map), else the
// "upper memory" size.
static void heap_init(const MultibootInfo* mbi) {
    const uint64_t k0 = (uintptr_t)__kernel_start & ~0xFFFull;
    const uint64_t k1 = ((uintptr_t)__kernel_end + 0xFFF) & ~0xFFFull;
    // A module (the disk image GRUB or the UEFI loader hands over) stays put
    // until storage_init has copied it.
    uint64_t m0 = 0, m1 = 0;
    if ((mbi->flags & (1 << 3)) && mbi->mods_count) {
        const uint32_t* mod = (const uint32_t*)(uintptr_t)mbi->mods_addr;
        m0 = mod[0] & ~0xFFFull;
        m1 = ((uint64_t)mod[1] + 0xFFF) & ~0xFFFull;
    }
    uint64_t total = 0;
    auto add = [&](uint64_t a, uint64_t e) {
        if (e > phys_limit) e = phys_limit;
        if (e > (uint64_t)(uintptr_t)-1) e = (uint64_t)(uintptr_t)-1;   // i386: 32-bit pointers
        if (a < 0x100000) a = 0x100000;
        if (a < k1 && e > k0) {                              // skip the kernel image
            if (a < k0) { heap_add((void*)(uintptr_t)a, (size_t)(k0 - a)); total += k0 - a; }
            a = k1;
        }
        if (a < m1 && e > m0) {                              // and the disk image module
            if (a < m0) { heap_add((void*)(uintptr_t)a, (size_t)(m0 - a)); total += m0 - a; }
            a = m1;
        }
        if (e > a) { heap_add((void*)(uintptr_t)a, (size_t)(e - a)); total += e - a; }
    };
    if (mbi->flags & (1 << 6)) {
        uintptr_t p = mbi->mmap_addr, end = p + mbi->mmap_length;
        // Copy the map first: the heap may be handed the memory it sits in.
        static MultibootMmap map[128];
        int n = 0;
        while (p < end && n < 128) {
            const MultibootMmap* m = (const MultibootMmap*)p;
            map[n++] = *m;
            p += m->size + 4;
        }
        for (int i = 0; i < n; i++)
            if (map[i].type == 1) add(map[i].addr, map[i].addr + map[i].len);
    } else if (mbi->flags & 1) {
        add(0x100000, 0x100000 + (uint64_t)mbi->mem_upper * 1024);
    }
    printf("Heap: %lu MB of RAM\n", (unsigned long)(total >> 20));
}

// ---------------------------------------------------------------- video
// The mode to set when no boot loader set one (QEMU -kernel, via Bochs VBE):
// VGA's 640x480, where the 80x25 text screen and every SCREEN mode fit.
#ifndef FB_W
#define FB_W 640
#define FB_H 480
#define FB_BPP 32
#endif

// BASIC's screen is drawn a strip at a time (video_band) as 0xRRGGBB and
// converted to whatever the framebuffer is: 8 (palettized), 15, 16, 24 or 32
// bits per pixel. Strips, not a whole back buffer: 640x480 at 32 bits would
// be 1.2 MB, a seventh of an 8 MB 386.
static uint8_t* fb;
static uint32_t fb_w, fb_h, fb_pitch;   // pitch in bytes
static int fb_bytes = 4;                // per pixel
static bool fb_indexed;
static uint8_t r_pos = 16, r_len = 8, g_pos = 8, g_len = 8, b_pos = 0, b_len = 8;
static uint32_t* band;                  // fb_w x band_cap rows
static int band_cap, band_y, band_h;
static uint32_t* row_sum;               // per screen row, as last copied out
static bool sums_valid;
static bool video_ready;
uint32_t fb_width() { return fb_w; }
uint32_t fb_height() { return fb_h; }

// 8-bit modes: the first 192 colours to appear get exact palette entries
// (VGA DAC); after that, colours map onto a fixed 4x4x4 cube in entries
// 192..255 (anti-aliased text can make more shades than a palette holds).
static uint32_t pal_key[1024];          // colour -> entry, open addressing
static uint8_t pal_val[1024];
static int pal_n, pal_keys;
static void dac_set(int i, uint32_t c) {
    outb(0x3C8, (uint8_t)i);
    outb(0x3C9, (c >> 18) & 63); outb(0x3C9, (c >> 10) & 63); outb(0x3C9, (c >> 2) & 63);
}
static uint8_t pal_index(uint32_t c) {
    uint32_t h = (c * 2654435761u) >> 22;
    for (;; h = (h + 1) & 1023) {
        if (pal_key[h] == c) return pal_val[h];
        if (pal_key[h] == 0xFFFFFFFF) break;
    }
    uint8_t idx;
    if (pal_n < 192) { idx = (uint8_t)pal_n++; dac_set(idx, c); }
    else idx = (uint8_t)(192 + ((c >> 22) & 3) * 16 + ((c >> 14) & 3) * 4 + ((c >> 6) & 3));
    if (pal_keys < 900) { pal_key[h] = c; pal_val[h] = idx; pal_keys++; }
    return idx;
}
static void pal_reset() {
    for (auto& k : pal_key) k = 0xFFFFFFFF;
    pal_n = pal_keys = 0;
    for (int i = 0; i < 64; i++)                          // the cube: 0, 85, 170, 255 per channel
        dac_set(192 + i, (uint32_t)((i >> 4) * 85) << 16 | (uint32_t)(((i >> 2) & 3) * 85) << 8 | (uint32_t)((i & 3) * 85));
    pal_index(0x000000);                                  // entry 0: black
}
static uint32_t native(uint32_t c) {
    c &= 0xFFFFFF;
    if (fb_indexed) return pal_index(c);
    uint32_t r = c >> 16, g = (c >> 8) & 255, b = c & 255;
    return (r >> (8 - r_len)) << r_pos | (g >> (8 - g_len)) << g_pos | (b >> (8 - b_len)) << b_pos;
}
// Video memory sits on a slow bus (ISA on a 386): move dwords.
static void copy_out(void* d, const void* s, size_t n) {
    size_t words = n / 4, rest = n % 4;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(words) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(rest) :: "memory");
}

static bool bga_init(uint32_t w, uint32_t h, uint32_t bpp) {
    uint32_t base = 0;
    PciDevice vga;
    if (pci_find_id(0x1234, 0x1111, &vga))                  // QEMU/Bochs std VGA
        base = pci_read(vga, 0x10) & 0xFFFFFFF0;
    outw(0x1CE, 0); if (!base || inw(0x1CF) < 0xB0C0) return false;
    auto w16 = [](uint16_t i, uint16_t v) { outw(0x1CE, i); outw(0x1CF, v); };
    w16(4, 0); w16(1, w); w16(2, h); w16(3, bpp); w16(4, 0x41);
    fb = (uint8_t*)(uintptr_t)base;
    fb_bytes = (bpp + 7) / 8;
    fb_w = w; fb_h = h; fb_pitch = w * fb_bytes;
    fb_indexed = bpp == 8;
    if (bpp == 16) { r_pos = 11; r_len = 5; g_pos = 5; g_len = 6; b_pos = 0; b_len = 5; }
    if (bpp == 15) { r_pos = 10; r_len = 5; g_pos = 5; g_len = 5; b_pos = 0; b_len = 5; }
    return true;
}

static bool video_init(const MultibootInfo* mbi) {
    bool ok = false;
    if ((mbi->flags & (1 << 12)) && mbi->fb_addr < phys_limit) {
        int bpp = mbi->fb_bpp;
        if (mbi->fb_type == 0 && bpp == 8) {
            fb_indexed = ok = true;
        } else if (mbi->fb_type == 1 && (bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32)) {
            const uint8_t* c = mbi->fb_color;
            r_pos = c[0]; r_len = c[1]; g_pos = c[2]; g_len = c[3]; b_pos = c[4]; b_len = c[5];
            ok = r_len && r_len <= 8 && g_len && g_len <= 8 && b_len && b_len <= 8;
        }
        if (ok) {
            fb = (uint8_t*)(uintptr_t)mbi->fb_addr;
            fb_w = mbi->fb_width; fb_h = mbi->fb_height; fb_pitch = mbi->fb_pitch;
            fb_bytes = (bpp + 7) / 8;
            printf("Using bootloader framebuffer %ux%u, %d bits\n", fb_w, fb_h, bpp);
        }
    }
    if (!ok && bga_init(FB_W, FB_H, FB_BPP)) {
        printf("Using Bochs/QEMU VBE %ux%u, %d bits\n", FB_W, FB_H, FB_BPP);
        ok = true;
    }
    if (ok && fb_indexed) pal_reset();
    return ok;
}

// A strip of the screen to draw into: rows y .. y+h-1, fb_width() wide.
uint32_t* video_band(int y, int h) {
    if (h > band_cap) {
        uint32_t* nb = (uint32_t*)malloc((size_t)fb_w * h * 4);
        if (!nb) return nullptr;
        free(band);
        band = nb; band_cap = h;
    }
    band_y = y; band_h = h;
    return band;
}
void video_invalidate() { sums_valid = false; }

// Copy the strip out, skipping rows that haven't changed since they were
// last copied (a checksum per row): most of the screen is usually still.
void video_band_done() {
    static uint8_t* line;
    if (!row_sum) {
        row_sum = (uint32_t*)malloc(fb_h * sizeof(uint32_t));
        line = (uint8_t*)malloc((size_t)fb_w * 4);
        if (!row_sum || !line) return;
        sums_valid = false;
    }
    if (!sums_valid) { for (uint32_t i = 0; i < fb_h; i++) row_sum[i] = 0x12345678u; sums_valid = true; }
    for (int r = 0; r < band_h; r++) {
        uint32_t y = (uint32_t)(band_y + r);
        if (y >= fb_h) break;
        const uint32_t* src = &band[(size_t)r * fb_w];
        uint32_t sum = 0x811C9DC5u;
        for (uint32_t x = 0; x < fb_w; x++) sum = (sum ^ src[x]) * 16777619u;
        if (row_sum[y] == sum) continue;
        row_sum[y] = sum;
        uint8_t* dst = fb + y * fb_pitch;
        if (fb_bytes == 4 && !fb_indexed && r_pos == 16 && g_pos == 8 && b_pos == 0) {
            copy_out(dst, src, fb_w * 4);                  // already in the screen's format
            continue;
        }
        uint8_t* d = line;
        uint32_t last = 0xFFFFFFFF, v = 0;
        for (uint32_t x = 0; x < fb_w; x++) {
            uint32_t p = src[x];
            if (p != last) { last = p; v = native(p); }
            switch (fb_bytes) {
            case 1: *d++ = (uint8_t)v; break;
            case 2: *(uint16_t*)d = (uint16_t)v; d += 2; break;
            case 3: d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d += 3; break;
            default: *(uint32_t*)d = v; d += 4; break;
            }
        }
        copy_out(dst, line, fb_w * fb_bytes);
    }
}

// ---------------------------------------------------------------- interrupts
#ifdef __x86_64__
struct __attribute__((packed)) IdtEntry {
    uint16_t off_lo, sel; uint8_t ist, type; uint16_t off_mid; uint32_t off_hi, zero;
};
#else
struct __attribute__((packed)) IdtEntry {         // 32-bit interrupt gate
    uint16_t off_lo, sel; uint8_t zero, type; uint16_t off_hi;
};
#endif
static IdtEntry idt[256];
extern "C" void isr_timer(), isr_keyboard(), isr_mouse(), isr_spurious(), isr_fault();

static void set_gate(int n, void (*h)()) {
    uintptr_t a = (uintptr_t)h;
#ifdef __x86_64__
    idt[n] = { (uint16_t)a, 0x08, 0, 0x8E, (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0 };
#else
    idt[n] = { (uint16_t)a, 0x08, 0, 0x8E, (uint16_t)(a >> 16) };
#endif
}

extern volatile uint32_t ticks, fine_ticks;
extern volatile uint8_t kbd_buf[256];
extern volatile uint8_t kbd_head, kbd_tail;
#define TICK_HZ 60
#define PIT_HZ  1193182u
#define PIT_DIV (PIT_HZ / (TICK_HZ * 4))

static void interrupts_init() {
    for (int i = 0; i < 32; i++) set_gate(i, isr_fault);
    for (int i = 32; i < 256; i++) set_gate(i, isr_spurious);
    set_gate(32, isr_timer);
    set_gate(33, isr_keyboard);
    set_gate(44, isr_mouse);
    struct __attribute__((packed)) { uint16_t lim; uintptr_t base; } idtr = { sizeof(idt) - 1, (uintptr_t)idt };
    __asm__ volatile("lidt %0" ::"m"(idtr));

    // Remap the PICs to vectors 32..47; unmask the timer, keyboard, the
    // cascade to the second PIC, and the PS/2 mouse (IRQ 12) on it.
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 32);   outb(0xA1, 40);
    outb(0x21, 4);    outb(0xA1, 2);
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xF8); outb(0xA1, 0xEF);

    // Mode 2 (rate generator): the count runs down once per interrupt, so
    // platform_us() can read how far into the tick we are.
    outb(0x43, 0x34); outb(0x40, PIT_DIV & 0xFF); outb(0x40, PIT_DIV >> 8);   // see irq.cpp: 240 Hz, ticks at 60

    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
    __asm__ volatile("sti");
}

void platform_reboot() {
    printf("Rebooting\n");
    __asm__ volatile("cli");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);                                  // i8042 pulse reset line
    outb(0xCF9, 0x02); outb(0xCF9, 0x06);              // PCI reset control
    struct __attribute__((packed)) { uint16_t lim; uintptr_t base; } none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" ::"m"(none));     // triple fault
    for (;;) __asm__ volatile("hlt");
}

// ---------------------------------------------------------------- keyboard
// PS/2 (IRQ 1, irq.cpp) and USB keyboards queue set-1 scancodes; this turns
// them into characters for BASIC (US layout), like the SDL build's keys:
// ASCII, '\r' for Enter, 8 Backspace, 27 Esc, 0x1000-0x1003 the arrows,
// KEY_EXT(scan code) the function keys, Home, End, PgUp, PgDn, Ins and Del.
// Queue a scancode from a source other than the PS/2 interrupt (USB).
void kbd_push(uint8_t b) {
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    kbd_buf[kbd_head] = b;
    kbd_head = kbd_head + 1;
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}

static bool shift_l, shift_r, ctrl, alt, caps, numlock = true;

static void key_event(bool ext, uint8_t code, bool down) {
    switch (code) {
    case 0x2A: if (!ext) shift_l = down; return;
    case 0x36: if (!ext) shift_r = down; return;
    case 0x1D: ctrl = down; return;
    case 0x38: alt = down; return;
    }
    if (!down) return;
    bool shift = shift_l || shift_r;
    if (ext) {
        switch (code) {
        case 0x48: gfx_bm_key(0x1000); return;          // arrows
        case 0x50: gfx_bm_key(0x1001); return;
        case 0x4B: gfx_bm_key(0x1002); return;
        case 0x4D: gfx_bm_key(0x1003); return;
        case 0x1C: gfx_bm_key('\r'); return;            // keypad Enter
        case 0x35: gfx_bm_key('/'); return;             // keypad /
        case 0x53: if (ctrl && alt) platform_reboot(); gfx_bm_key(KEY_EXT(SCAN_DEL)); return;   // Delete
        case 0x47: gfx_bm_key(KEY_EXT(SCAN_HOME)); return;
        case 0x4F: gfx_bm_key(KEY_EXT(SCAN_END)); return;
        case 0x52: gfx_bm_key(KEY_EXT(SCAN_INS)); return;
        case 0x49: if (shift) gfx_bm_scroll(1); else gfx_bm_key(KEY_EXT(SCAN_PGUP)); return;   // Page Up
        case 0x51: if (shift) gfx_bm_scroll(-1); else gfx_bm_key(KEY_EXT(SCAN_PGDN)); return; // Page Down
        case 0x46: StandaloneBasic::g_break = 1; gfx_bm_key(3); return;   // Ctrl+Break
        }
        return;
    }
    switch (code) {
    case 0x3A: caps = !caps; return;
    case 0x45: numlock = !numlock; return;
    case 0x01: gfx_bm_key(27); return;
    case 0x0E: gfx_bm_key(8); return;
    case 0x0F: gfx_bm_key(9); return;
    case 0x1C: gfx_bm_key('\r'); return;
    case 0x57: gfx_bm_key(KEY_EXT(SCAN_F11)); return;
    case 0x58: gfx_bm_key(KEY_EXT(SCAN_F12)); return;
    }
    if (code >= 0x3B && code <= 0x44) { gfx_bm_key(KEY_EXT(code)); return; }   // F1-F10: scan 59-68
    if (code >= 0x47 && code <= 0x53) {                 // keypad
        static const char digits[] = "789-456+1230.";
        char d = digits[code - 0x47];
        if (numlock || d == '-' || d == '+') { gfx_bm_key(d); return; }
        if (code == 0x48) gfx_bm_key(0x1000);
        else if (code == 0x50) gfx_bm_key(0x1001);
        else if (code == 0x4B) gfx_bm_key(0x1002);
        else if (code == 0x4D) gfx_bm_key(0x1003);
        else if (code == 0x53) { if (ctrl && alt) platform_reboot(); gfx_bm_key(KEY_EXT(SCAN_DEL)); }
        else if (code == 0x47 || code == 0x49 || code == 0x4F || code == 0x51 || code == 0x52)
            gfx_bm_key(KEY_EXT(code));                  // Home PgUp End PgDn Ins
        return;
    }
    if (code == 0x37) { gfx_bm_key('*'); return; }      // keypad *
    static const char lower[] = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0\0\0 ";
    static const char upper[] = "\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP{}\0\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0\0\0 ";
    if (code >= sizeof lower - 1) return;
    char c = shift ? upper[code] : lower[code];
    if (!c) return;
    if (c >= 'a' && c <= 'z' && caps) c -= 32;
    else if (c >= 'A' && c <= 'Z' && caps) c += 32;
    if (ctrl) {
        char l = c | 32;
        if (l >= 'a' && l <= 'z') {
            // Ctrl+C copies selected text; with nothing selected it breaks
            // a running program. Ctrl+V types what was copied.
            if (l == 'c' && gfx_bm_copy()) return;
            if (l == 'v') { gfx_bm_paste(); return; }
            if (l == 'c') StandaloneBasic::g_break = 1;
            gfx_bm_key(l - 'a' + 1);
        }
        return;
    }
    gfx_bm_key((unsigned char)c);
}

// ---------------------------------------------------------------- mouse
// A PS/2 mouse on the i8042's second port (IRQ 12, irq.cpp) or a USB one
// (usb.cpp): either way relative motion and buttons go to mouse_push(),
// and on to the display (basic_gfx_bm.cpp), which keeps the pointer, draws
// it, selects text for copy and paste and answers BASIC's mouse functions.
extern volatile uint8_t mouse_buf[256];
extern volatile uint8_t mouse_head, mouse_tail;
volatile bool g_bm_selection;            // set by the display: Ctrl+C copies rather than breaks
static int ps2_packet = 3;               // bytes per packet: 4 with a wheel (IntelliMouse)

static bool i8042_wait_write() { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return true; return false; }
static bool i8042_wait_read()  { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return true; return false; }
static bool mouse_send(uint8_t b) {
    i8042_wait_write(); outb(0x64, 0xD4);              // next byte to the aux device
    i8042_wait_write(); outb(0x60, b);
    for (int tries = 0; tries < 4; tries++) {          // skip stray bytes until the ACK
        if (!i8042_wait_read()) return false;
        if (inb(0x60) == 0xFA) return true;
    }
    return false;
}
static void ps2_mouse_init() {
    i8042_wait_write(); outb(0x64, 0xA8);              // enable the aux port
    i8042_wait_write(); outb(0x64, 0x20);              // read the controller config
    if (!i8042_wait_read()) { printf("PS/2 mouse: none\n"); return; }
    uint8_t cfg = inb(0x60);
    cfg = (cfg | 0x02) & ~0x20;                        // aux interrupt on, aux clock on
    i8042_wait_write(); outb(0x64, 0x60);
    i8042_wait_write(); outb(0x60, cfg);
    if (!mouse_send(0xF6)) { printf("PS/2 mouse: none\n"); return; }   // defaults
    // The IntelliMouse knock (sample rates 200, 100, 80) turns on the wheel:
    // the mouse then answers ID 3 and sends 4-byte packets.
    static const uint8_t knock[] = {200, 100, 80};
    bool ok = true;
    for (uint8_t r : knock) ok = ok && mouse_send(0xF3) && mouse_send(r);
    if (ok && mouse_send(0xF2) && i8042_wait_read() && inb(0x60) == 3) ps2_packet = 4;
    mouse_send(0xF3); mouse_send(100);                 // a normal sample rate again
    if (!mouse_send(0xF4)) { printf("PS/2 mouse: none\n"); return; }   // start streaming
    printf("PS/2 mouse: ready%s\n", ps2_packet == 4 ? " (with wheel)" : "");
}

// Relative motion (x right, y down), buttons (1 left, 2 right, 4 middle)
// and wheel clicks (+ away from you), from either kind of mouse.
void mouse_push(int dx, int dy, int buttons, int wheel) { gfx_bm_mouse(dx, dy, buttons, wheel); }

static void poll_ps2_mouse() {
    static uint8_t pkt[4];
    static int n;
    while (mouse_tail != mouse_head) {
        uint8_t b = mouse_buf[mouse_tail++];
        if (n == 0 && !(b & 0x08)) continue;           // resync: byte 0 always has bit 3 set
        pkt[n++] = b;
        if (n < ps2_packet) continue;
        n = 0;
        if (pkt[0] & 0xC0) continue;                   // overflow: drop the packet
        int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
        int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
        int z = ps2_packet == 4 ? (int)(int8_t)(pkt[3] << 4) >> 4 : 0;   // 4-bit signed
        mouse_push(dx, -dy, pkt[0] & 7, -z);          // PS/2: y grows upward, z toward you
    }
}

void platform_poll_input() {
    usb_poll();
    poll_ps2_mouse();
    static bool ext;
    while (kbd_tail != kbd_head) {
        uint8_t b = kbd_buf[kbd_tail++];
        if (b == 0xE0) { ext = true; continue; }
        if (b == 0xE1) { ext = false; continue; }
        bool e = ext;
        ext = false;
        if (e && ((b & 0x7F) == 0x2A || (b & 0x7F) == 0x36)) continue;   // fake shifts around E0 keys
        key_event(e, b & 0x7F, !(b & 0x80));
    }
}

// ---------------------------------------------------------------- time
// The PIT runs at 240 Hz (irq.cpp), which also drives the note player.
uint32_t platform_ms() { return (uint32_t)((uint64_t)fine_ticks * 1000 / (TICK_HZ * 4)); }

// Microseconds since boot: the ticks so far plus how far the PIT has
// counted into the current one. TIMER needs this -- at 4 ms a tick, a
// program that times a short loop (Nibbles does, to set its speed) would
// see no time pass and divide by zero.
uint64_t platform_us() {
    static uint64_t last;
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t t = fine_ticks;
    outb(0x43, 0x00);                                  // latch channel 0's count
    uint8_t lo = inb(0x40), hi = inb(0x40);
    outb(0x20, 0x0A);                                  // read the PIC's request register:
    bool pending = inb(0x20) & 1;                      // a tick not yet counted?
    __asm__ volatile("push %0; popf" :: "r"(fl) : "memory", "cc");
    uint32_t cnt = (uint32_t)lo | ((uint32_t)hi << 8);
    uint32_t into = cnt <= PIT_DIV ? PIT_DIV - cnt : 0;
    if (pending && into < PIT_DIV / 2) t++;            // the count wrapped before we read it
    uint64_t us = (uint64_t)t * 1000000u / (TICK_HZ * 4) + (uint64_t)into * 1000000u / PIT_HZ;
    if (us < last) us = last;                          // never run backwards
    last = us;
    return us;
}

// Waiting: keep the keyboard drained and let the CPU sleep between ticks.
static void wait_ms(uint32_t ms, bool render) {
    uint32_t t0 = platform_ms(), last_render = t0;
    for (;;) {
        platform_poll_input();
        uint32_t now = platform_ms();
        if (render && now - last_render >= 16) { gfx_sdl_render(); last_render = now; }
        if (now - t0 >= ms || (render && StandaloneBasic::g_break)) break;
        __asm__ volatile("hlt");
    }
}
// SLEEP / nanosleep: the screen keeps updating, and Ctrl+C ends it early.
void platform_sleep_ms(uint32_t ms) { gfx_sdl_render(); wait_ms(ms, true); }

extern "C" {
Uint32 SDL_GetTicks(void) { return platform_ms(); }
void SDL_Delay(Uint32 ms) { wait_ms(ms, false); }
Uint64 SDL_GetPerformanceCounter(void) { return platform_us(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000; }
void SDL_Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
}

// ---------------------------------------------------------------- console
// stdout/stderr (error messages, LIST to the screen...) and stdin.
void console_write(const char* s, size_t n) {
    char buf[256];
    while (n) {
        size_t k = n < sizeof buf - 1 ? n : sizeof buf - 1;
        memcpy(buf, s, k);
        buf[k] = 0;
        for (size_t i = 0; i < k; i++) serial_putc(buf[i]);
        if (video_ready) StandaloneBasic::display_print(buf);
        s += k; n -= k;
    }
}
int console_getchar() { return StandaloneBasic::display_getchar(); }

// ---------------------------------------------------------------- main
static jmp_buf restart;
static bool basic_running;

// exit() from the interpreter (an unrecoverable error): back to a fresh
// BASIC prompt rather than a dead machine.
extern "C" void exit(int code) noexcept {
    printf("exit(%d)\n", code);
    if (basic_running) longjmp(restart, 1);
    platform_reboot();
    __builtin_unreachable();
}

static void halt_screen(const char* l1, const char* l2) {
    printf("%s\n%s\n", l1, l2);
    for (uint32_t y = 0; y < fb_h; y += 8) {
        uint32_t* b = video_band((int)y, 8);
        if (!b) break;
        memset(b, 0, (size_t)fb_w * 8 * 4);
        const char* lines[2] = {l1, l2};
        for (int l = 0; l < 2; l++)
            for (int i = 0; lines[l][i]; i++) {
                char ch = lines[l][i] < 32 || lines[l][i] > 126 ? '?' : lines[l][i];
                const unsigned char* g = font8x16[ch - 32];
                for (int r = 0; r < 16; r++)
                    for (int bit = 0; bit < 8; bit++) {
                        uint32_t px = 16 + i * 8 + bit, py = 16 + l * 24 + r;
                        if ((g[r] & (0x80 >> bit)) && px < fb_w && py >= y && py < y + 8)
                            b[(py - y) * fb_w + px] = l ? 0xAAAAAA : 0xFF5555;
                    }
            }
        video_band_done();
    }
    for (;;) __asm__ volatile("cli; hlt");
}

extern "C" void (*__init_array_start[])(), (*__init_array_end[])();
#ifndef __x86_64__
extern "C" uint32_t fpu_present;                       // boot32.S
#endif

extern "C" void kmain() {
    serial_init();
    printf("\nFelix BASIC - bare metal\n");
    const MultibootInfo* mbi = (const MultibootInfo*)(uintptr_t)mb_info;
    if (mb_magic != 0x2BADB002) { printf("Not booted by a Multiboot loader\n"); return; }
    static MultibootInfo info;
    info = *mbi;
    static char cmdbuf[512];
    const char* cmdline = nullptr;
    if (info.flags & (1 << 2)) {
        strncpy(cmdbuf, (const char*)(uintptr_t)info.cmdline, sizeof(cmdbuf) - 1);
        cmdline = cmdbuf;
    }
    // The heap before the constructors: the interpreter allocates its
    // variable and program tables in them.
    heap_init(&info);
    if (!video_init(&info)) { printf("No usable framebuffer found\n"); return; }
    video_ready = true;
    // The interpreter's constructors allocate its variable table (MAX_VARS
    // variables; arrays come from the heap at DIM) and don't
    // survive running out: check first, with room for the rest.
    // Room for that and the interpreter's other tables, with 1 MB to spare
    // for the program; arrays and graphics pages that don't fit give
    // "Out of memory" (a 640x480 page is 1.2 MB).
    size_t need = sizeof(StandaloneBasic::Var) * MAX_VARS + (1u << 20);
    if (heap_free_bytes() < need) {
        static char l2[96];
        size_t base = (1u << 20) + (size_t)(__kernel_end - __kernel_start);   // below 1 MB, the kernel
        snprintf(l2, sizeof l2, "Felix BASIC needs %u MB of RAM; this PC has %u MB.",
                 (unsigned)((need + base + (1u << 20) - 1) >> 20),
                 (unsigned)((heap_free_bytes() + base + (512u << 10)) >> 20));
        halt_screen("NOT ENOUGH MEMORY", l2);
    }
    for (auto f = __init_array_start; f != __init_array_end; f++) (*f)();

#ifndef __x86_64__
    if (!fpu_present)                                   // BASIC numbers are doubles
        halt_screen("FELIX BASIC NEEDS A MATH COPROCESSOR",
                    "This PC has no 387 (or 486DX) floating-point unit.");
#endif

    bool speaker = false;
    for (const char* p = cmdline; p && *p; p++)
        if (!strncmp(p, "audio=speaker", 13)) speaker = true;
    AudioDriver drv = speaker ? AUDIO_NONE : audio_init(cmdline);
    player_init(drv != AUDIO_NONE);
    printf("Sound: %s\n", drv != AUDIO_NONE ? audio_name() : "PC speaker");
    usb_init(cmdline);
    ps2_mouse_init();
    interrupts_init();
    if ((info.flags & (1 << 3)) && info.mods_count) {   // a disk image (UEFI loader)
        const uint32_t* mod = (const uint32_t*)(uintptr_t)info.mods_addr;
        storage_set_image((const void*)(uintptr_t)mod[0], mod[1] - mod[0]);
    }
    storage_init(info.flags, info.boot_device, cmdline);
    printf("Heap after start-up: %lu KB free\n", (unsigned long)(heap_free_bytes() >> 10));

    static char arg0[] = "basic";
    static char* argv[] = {arg0, nullptr};
    for (;;) {
        if (setjmp(restart) == 0) {
            basic_running = true;
            main(1, argv);
        }
        basic_running = false;
        player_stop();
        // SYSTEM (or an error the interpreter couldn't recover from).
        StandaloneBasic::display_print((char*)"\nReboot the computer (Y/N)? ");
        gfx_sdl_render();
        int c;
        do c = StandaloneBasic::display_getchar(); while (c != 'y' && c != 'Y' && c != 'n' && c != 'N' && c != '\r' && c != 27);
        if (c == 'y' || c == 'Y') platform_reboot();
    }
}

extern "C" void fault_handler() {
    printf("CPU exception - halted\n");
}
