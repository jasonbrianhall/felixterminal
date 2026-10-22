// MS-DOS (DJGPP) platform layer for Felix BASIC.
//
// Stands in for baremetal/kernel.cpp: the same interface (video.hpp and the
// SDL2 stand-in in baremetal/overrides) so that basic_gfx_bm.cpp, generated
// from ../basic_gfx_sdl.cpp, runs unchanged. DOS and DJGPP supply the C
// library, files and memory; this file supplies:
//
//   video     VESA VBE 2.0 linear framebuffer (8/15/16/24/32 bpp), a banked
//             VBE 1.2 window when there is no LFB, or plain VGA mode 12h
//             (640x480, 16 colours) when there is no VESA BIOS at all.
//             FELIXVID=WxHxBPP (e.g. 800x600x16) picks a VESA mode.
//   keyboard  BIOS (INT 16h); Ctrl+C / Ctrl+Break stop a running program
//   mouse     INT 33h driver (CuteMouse wheel API if present)
//   time      uclock() (PIT), SDL_GetTicks / SDL_Delay
//   console   stdout/stderr go to the BASIC screen (File System Extension)
#define FELIX_DOS_PLATFORM
#undef main
#include <SDL2/SDL.h>
#include "video.hpp"
#include "basic_ns.h"
#include "basic.h"
#include "display.h"
#include "sound_player.hpp"

#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <sys/exceptn.h>
#include <sys/fsext.h>
#include <bios.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void gfx_sdl_render();
bool gfx_sdl_pump();
bool player_active();                    // dos_sound.cpp: cheap check, no clock read

// ---------------------------------------------------------------- log
// Start-up messages, kept for DMESG.
static char klog[8192];
static size_t klog_len;
static void klog_add(const char* s) {
    for (; *s; s++) klog[klog_len++ % sizeof klog] = *s;
}
static void klogf(const char* fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    klog_add(b);
}
extern "C" const char* platform_boot_log(void) {
    static char out[sizeof klog + 1];
    size_t n = klog_len < sizeof klog ? klog_len : sizeof klog;
    size_t start = klog_len - n;
    for (size_t i = 0; i < n; i++) out[i] = klog[(start + i) % sizeof klog];
    out[n] = 0;
    char* p = out;
    if (klog_len > sizeof klog) while (*p && *p != '\n') p++;
    return *p == '\n' ? p + 1 : p;
}

// ---------------------------------------------------------------- memory
extern "C" size_t heap_largest_free(void) {
    __dpmi_free_mem_info fi;
    if (__dpmi_get_free_memory_information(&fi) != 0) return 64u << 20;
    unsigned long b = fi.largest_available_free_block_in_bytes;
    if (b == 0xFFFFFFFFul) return 64u << 20;
    return (size_t)b;
}

// ---------------------------------------------------------------- video
enum VMode { V_NONE, V_LFB, V_BANKED, V_VGA12 };
static VMode vmode = V_NONE;
static uint32_t fb_w, fb_h, fb_pitch;
static int fb_bytes = 4;
static bool fb_indexed;
static uint8_t r_pos = 16, r_len = 8, g_pos = 8, g_len = 8, b_pos = 0, b_len = 8;
static int lfb_sel = -1;                 // LDT selector over the linear framebuffer
static __dpmi_meminfo lfb_map;
static uint32_t win_seg = 0xA000, win_gran = 65536, win_size = 65536;   // banked
static int cur_bank = -1;
static uint32_t* band;
static int band_cap, band_y, band_h;
static uint32_t* row_sum;
static bool sums_valid;
static bool video_ready;

uint32_t fb_width() { return fb_w; }
uint32_t fb_height() { return fb_h; }

// 8-bit palette: as kernel.cpp -- the first 192 colours exact, then a cube.
static uint32_t pal_key[1024];
static uint8_t pal_val[1024];
static int pal_n, pal_keys;
static void dac_set(int i, uint32_t c) {
    outportb(0x3C8, (uint8_t)i);
    outportb(0x3C9, (c >> 18) & 63); outportb(0x3C9, (c >> 10) & 63); outportb(0x3C9, (c >> 2) & 63);
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
    for (int i = 0; i < 64; i++)
        dac_set(192 + i, (uint32_t)((i >> 4) * 85) << 16 | (uint32_t)(((i >> 2) & 3) * 85) << 8 | (uint32_t)((i & 3) * 85));
    pal_index(0x000000);
}

// Mode 12h: the nearest of the 16 standard colours (cached).
static const uint32_t ega16[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF };
static uint8_t nearest16(uint32_t c) {
    static uint32_t ck[512]; static uint8_t cv[512]; static bool init;
    if (!init) { for (auto& k : ck) k = 0xFFFFFFFF; init = true; }
    uint32_t h = (c * 2654435761u) >> 23;
    if (ck[h] == c) return cv[h];
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255, best = 0;
    long bd = 1L << 30;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)((ega16[i] >> 16) & 255), dg = g - (int)((ega16[i] >> 8) & 255), db = b - (int)(ega16[i] & 255);
        long d = 3L * dr * dr + 4L * dg * dg + 2L * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    ck[h] = c; cv[h] = (uint8_t)best;
    return (uint8_t)best;
}

static uint32_t native(uint32_t c) {
    c &= 0xFFFFFF;
    if (fb_indexed) return pal_index(c);
    uint32_t r = c >> 16, g = (c >> 8) & 255, b = c & 255;
    return (r >> (8 - r_len)) << r_pos | (g >> (8 - g_len)) << g_pos | (b >> (8 - b_len)) << b_pos;
}

static void set_bank(int bank) {
    if (bank == cur_bank) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F05; r.x.bx = 0; r.x.dx = (unsigned short)bank;
    __dpmi_int(0x10, &r);
    cur_bank = bank;
}

// Copy n bytes to framebuffer offset `off`.
static void fb_write(uint32_t off, const void* src, uint32_t n) {
    if (vmode == V_LFB) {
        movedata(_my_ds(), (unsigned)src, lfb_sel, off, n);
        return;
    }
    const uint8_t* s = (const uint8_t*)src;
    while (n) {
        int bank = (int)(off / win_gran);
        uint32_t in = off - (uint32_t)bank * win_gran;
        uint32_t room = win_size - in;
        uint32_t k = n < room ? n : room;
        set_bank(bank);
        dosmemput(s, k, win_seg * 16 + in);
        off += k; s += k; n -= k;
    }
}

static void vga12_row(uint32_t y, const uint32_t* src) {
    uint8_t planes[4][80];
    memset(planes, 0, sizeof planes);
    uint32_t last = 0xFFFFFFFF; uint8_t v = 0;
    for (uint32_t x = 0; x < 640; x++) {
        uint32_t p = src[x] & 0xFFFFFF;
        if (p != last) { last = p; v = nearest16(p); }
        uint8_t bit = (uint8_t)(0x80 >> (x & 7));
        for (int pl = 0; pl < 4; pl++) if (v & (1 << pl)) planes[pl][x >> 3] |= bit;
    }
    for (int pl = 0; pl < 4; pl++) {
        outportw(0x3C4, (uint16_t)(((1 << pl) << 8) | 0x02));     // map mask
        dosmemput(planes[pl], 80, 0xA0000 + y * 80);
    }
    outportw(0x3C4, 0x0F02);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool vbe_mode_info(int mode, uint8_t mi[256]) {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F01; r.x.cx = (unsigned short)mode;
    r.x.es = __tb >> 4; r.x.di = __tb & 15;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    dosmemget(__tb, 256, mi);
    return true;
}

static bool vesa_init() {
    uint8_t info[512];
    memset(info, 0, sizeof info);
    memcpy(info, "VBE2", 4);
    dosmemput(info, sizeof info, __tb);
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F00; r.x.es = __tb >> 4; r.x.di = __tb & 15;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    dosmemget(__tb, sizeof info, info);
    if (memcmp(info, "VESA", 4) != 0) return false;
    int ver = rd16(info + 4);
    uint32_t lp = rd32(info + 14);
    uint32_t list_lin = ((lp >> 16) << 4) + (lp & 0xFFFF);
    uint16_t modes[256];
    int nm = 0;
    for (; nm < 255; nm++) {
        uint16_t m = _farpeekw(_dos_ds, list_lin + nm * 2);
        if (m == 0xFFFF) break;
        modes[nm] = m;
    }
    klogf("VESA BIOS %d.%d, %d modes\n", ver >> 8, ver & 255, nm);

    // Wanted: FELIXVID=WxHxBPP, otherwise 640x480, bpp in order of preference.
    int want_w = 640, want_h = 480, want_bpp = 0;
    if (const char* e = getenv("FELIXVID")) sscanf(e, "%dx%dx%d", &want_w, &want_h, &want_bpp);
    static const int pref[] = {16, 15, 32, 24, 8};
    int best = -1, best_score = -1;
    uint8_t mi[256];
    for (int i = 0; i < nm; i++) {
        if (!vbe_mode_info(modes[i], mi)) continue;
        uint16_t attr = rd16(mi);
        if ((attr & 0x19) != 0x19) continue;            // supported, colour, graphics
        int w = rd16(mi + 18), h = rd16(mi + 20), bpp = mi[25], model = mi[27];
        if (w < 640 || h < 480) continue;
        if (bpp == 8 ? model != 4 : model != 6) continue;
        bool lfb = (attr & 0x80) && ver >= 0x200 && rd32(mi + 40);
        if (!lfb && (mi[2] & 5) != 5) continue;         // window A must be writable
        int score = 0;
        if (w == want_w && h == want_h) score += 1000;
        else score += 500 - (w - want_w) / 8 - (h - want_h) / 8;
        if (want_bpp) { if (bpp == want_bpp) score += 100; }
        else for (int k = 0; k < 5; k++) if (pref[k] == bpp) score += 100 - k * 10;
        if (lfb) score += 5;
        if (score > best_score) { best_score = score; best = modes[i]; }
    }
    if (best < 0 || !vbe_mode_info(best, mi)) return false;

    uint16_t attr = rd16(mi);
    fb_w = rd16(mi + 18); fb_h = rd16(mi + 20);
    int bpp = mi[25];
    fb_bytes = (bpp + 7) / 8;
    fb_indexed = bpp == 8;
    fb_pitch = rd16(mi + 16);
    if (!fb_indexed) {
        r_len = mi[31]; r_pos = mi[32]; g_len = mi[33]; g_pos = mi[34]; b_len = mi[35]; b_pos = mi[36];
        if (!r_len || !g_len || !b_len) {               // VBE 1.x leaves the masks out
            if (bpp == 15) { r_pos = 10; r_len = 5; g_pos = 5; g_len = 5; b_pos = 0; b_len = 5; }
            else if (bpp == 16) { r_pos = 11; r_len = 5; g_pos = 5; g_len = 6; b_pos = 0; b_len = 5; }
            else { r_pos = 16; r_len = 8; g_pos = 8; g_len = 8; b_pos = 0; b_len = 8; }
        }
    }
    bool lfb = (attr & 0x80) && ver >= 0x200 && rd32(mi + 40);
    if (lfb) {
        if (ver >= 0x300 && rd16(mi + 50)) fb_pitch = rd16(mi + 50);   // LinBytesPerScanLine
        lfb_map.address = rd32(mi + 40);
        lfb_map.size = (fb_pitch * fb_h + 0xFFF) & ~0xFFFu;
        if (__dpmi_physical_address_mapping(&lfb_map) != 0) lfb = false;
        else {
            lfb_sel = __dpmi_allocate_ldt_descriptors(1);
            if (lfb_sel < 0) lfb = false;
            else {
                __dpmi_set_segment_base_address(lfb_sel, lfb_map.address);
                __dpmi_set_segment_limit(lfb_sel, (lfb_map.size - 1) | 0xFFF);
            }
        }
    }
    if (!lfb) {
        if ((mi[2] & 5) != 5) return false;
        win_gran = (uint32_t)rd16(mi + 4) * 1024;
        win_size = (uint32_t)rd16(mi + 6) * 1024;
        win_seg = rd16(mi + 8);
        if (!win_gran || !win_size || !win_seg) return false;
    }
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F02; r.x.bx = (unsigned short)(best | (lfb ? 0x4000 : 0));
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    vmode = lfb ? V_LFB : V_BANKED;
    cur_bank = -1;
    klogf("Video: VESA mode %Xh, %ux%u, %d bits, %s\n", best, fb_w, fb_h, bpp,
          lfb ? "linear framebuffer" : "banked window");
    return true;
}

static bool vga12_init() {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0012;
    __dpmi_int(0x10, &r);
    fb_w = 640; fb_h = 480; fb_pitch = 80; fb_bytes = 1; fb_indexed = false;
    vmode = V_VGA12;
    klogf("Video: no VESA BIOS; VGA mode 12h, 640x480, 16 colours\n");
    return true;
}

static void text_mode() {
    if (vmode == V_NONE) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
    vmode = V_NONE;
    video_ready = false;
}

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

void video_band_done() {
    static uint8_t* line;
    if (!row_sum) {
        row_sum = (uint32_t*)malloc(fb_h * sizeof(uint32_t));
        line = (uint8_t*)malloc((size_t)fb_w * 4);
        if (!row_sum || !line) return;
        sums_valid = false;
    }
    if (!sums_valid) { for (uint32_t i = 0; i < fb_h; i++) row_sum[i] = 0x12345678u; sums_valid = true; }
    for (int rr = 0; rr < band_h; rr++) {
        uint32_t y = (uint32_t)(band_y + rr);
        if (y >= fb_h) break;
        const uint32_t* src = &band[(size_t)rr * fb_w];
        uint32_t sum = 0x811C9DC5u;
        for (uint32_t x = 0; x < fb_w; x++) sum = (sum ^ src[x]) * 16777619u;
        if (row_sum[y] == sum) continue;
        row_sum[y] = sum;
        if (vmode == V_VGA12) { vga12_row(y, src); continue; }
        if (fb_bytes == 4 && !fb_indexed && r_pos == 16 && g_pos == 8 && b_pos == 0) {
            fb_write(y * fb_pitch, src, fb_w * 4);
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
        fb_write(y * fb_pitch, line, fb_w * fb_bytes);
    }
}

// ---------------------------------------------------------------- time
static uclock_t t_start;
static uint64_t now_us() {
    uclock_t t = uclock() - t_start;
    return (uint64_t)t * 1000000ull / UCLOCKS_PER_SEC;
}
static void poll_all();

// Frames at most every dos_frame_ms (patch_gfx_dos.py); one held back is
// drawn while waiting.
unsigned dos_frame_ms = 40;
bool dos_render_deferred;

// The interpreter reads the clock after every statement. Reading the PIT
// (uclock) costs port I/O and a 64-bit divide, so between BIOS ticks
// (18.2 Hz, a plain memory read) the clock is read only every 8th call.
static uint32_t ticks_ms;
static unsigned ticks_calls;
static uint32_t ticks_bios;

extern "C" {
Uint32 SDL_GetTicks(void) {
    uint32_t b = _farpeekl(_dos_ds, 0x46C);
    if (++ticks_calls >= 8 || b != ticks_bios) {
        ticks_calls = 0; ticks_bios = b;
        ticks_ms = (uint32_t)(now_us() / 1000);
        poll_all();
    }
    return ticks_ms;
}
Uint64 SDL_GetPerformanceCounter(void) { return now_us(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000; }
void SDL_Delay(Uint32 ms) {
    uint64_t end = now_us() + (uint64_t)ms * 1000;
    for (;;) {
        ticks_calls = 8;                                 // fresh time for the frame cap
        platform_poll_input();
        if (dos_render_deferred) gfx_sdl_render();
        if (now_us() >= end) break;
        __dpmi_yield();
    }
}
void SDL_Log(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    klog_add(b); klog_add("\n");
}
}

// ---------------------------------------------------------------- keyboard
// BIOS keys (getxkey: ASCII, 0x100 + scan code, 0x200 + scan code for the
// grey keys) to the codes the display expects (see kernel.cpp).
static void key_in(int k) {
    int shift = bioskey(2) & 3;
    if (k >= 0x100) {
        int sc = k & 0xFF;
        switch (sc) {
        case 0x48: gfx_bm_key(0x1000); return;
        case 0x50: gfx_bm_key(0x1001); return;
        case 0x4B: gfx_bm_key(0x1002); return;
        case 0x4D: gfx_bm_key(0x1003); return;
        case 0x49: if (shift) gfx_bm_scroll(1); else gfx_bm_key(KEY_EXT(SCAN_PGUP)); return;
        case 0x51: if (shift) gfx_bm_scroll(-1); else gfx_bm_key(KEY_EXT(SCAN_PGDN)); return;
        case 0x47: case 0x4F: case 0x52: case 0x53:
            gfx_bm_key(KEY_EXT(sc)); return;
        case 0x85: case 0x86: gfx_bm_key(KEY_EXT(sc)); return;           // F11, F12
        }
        if (sc >= 0x3B && sc <= 0x44) { gfx_bm_key(KEY_EXT(sc)); return; }   // F1-F10
        if (k >= 0x200 && (k & 0xFF00) == 0x200) return;
        return;                                          // Alt+letter etc.: ignored
    }
    switch (k) {
    case 13: case 10: gfx_bm_key('\r'); return;
    case 3:                                              // Ctrl+C
        if (gfx_bm_copy()) return;
        StandaloneBasic::g_break = 1;
        gfx_bm_key(3);
        return;
    case 22: gfx_bm_paste(); return;                     // Ctrl+V
    }
    gfx_bm_key(k & 0xFF);
}

// ---------------------------------------------------------------- mouse
static bool mouse_ok, mouse_wheel;
static void mouse_init() {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0000;
    __dpmi_int(0x33, &r);
    mouse_ok = r.x.ax == 0xFFFF;
    if (!mouse_ok) { klogf("Mouse: none (load a mouse driver for one)\n"); return; }
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0011;                                     // CuteMouse wheel API
    __dpmi_int(0x33, &r);
    mouse_wheel = r.x.ax == 0x574D && (r.x.cx & 1);
    memset(&r, 0, sizeof r); r.x.ax = 0x000B; __dpmi_int(0x33, &r);   // reset the counters
    klogf("Mouse: %d buttons%s\n", r.x.bx == 0xFFFF ? 2 : 3, mouse_wheel ? ", wheel" : "");
}
static void mouse_poll() {
    if (!mouse_ok) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x000B;
    __dpmi_int(0x33, &r);
    int dx = (short)r.x.cx, dy = (short)r.x.dx;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;
    __dpmi_int(0x33, &r);
    int buttons = r.x.bx & 7;
    int wheel = mouse_wheel ? -(signed char)r.h.bh : 0;
    static int last_buttons;
    if (dx || dy || wheel || buttons != last_buttons) {
        last_buttons = buttons;
        gfx_bm_mouse(dx, dy / 2, buttons, wheel);         // mickeys: 8 per 8 px across, 16 down
    }
}

volatile bool g_bm_selection;

void platform_poll_input() {
    poll_all();
    // A key waiting? The BIOS buffer's head and tail (a memory read) before
    // the INT 16h, which means a switch to real mode.
    while (_farpeekw(_dos_ds, 0x41A) != _farpeekw(_dos_ds, 0x41C) && bioskey(0x11)) key_in(getxkey());
    if (_go32_was_ctrl_break_hit()) { StandaloneBasic::g_break = 1; gfx_bm_key(3); }
    static uint64_t last_mouse;
    uint64_t t = now_us();
    if (t - last_mouse >= 10000) { last_mouse = t; mouse_poll(); }   // INT 33h: at most 100 times a second
}

// The note player (dos_sound.cpp) is serviced whenever time is read.
static void poll_all() { if (player_active()) player_tick(); }

// ---------------------------------------------------------------- console
// printf and friends write to fd 1/2: show it on the BASIC screen.
static int con_hook(__FSEXT_Fnumber n, int* rv, va_list args) {
    if (n != __FSEXT_write || !video_ready) return 0;
    va_arg(args, int);
    const char* s = va_arg(args, const char*);
    size_t len = va_arg(args, size_t);
    char buf[257];
    size_t done = 0;
    while (done < len) {
        size_t k = len - done < 256 ? len - done : 256;
        memcpy(buf, s + done, k); buf[k] = 0;
        StandaloneBasic::display_print(buf);
        done += k;
    }
    *rv = (int)len;
    return 1;
}

// ---------------------------------------------------------------- start / exit
static void restore_screen() {
    player_stop();
    text_mode();
    if (lfb_sel >= 0) { __dpmi_free_ldt_descriptor(lfb_sel); lfb_sel = -1; }
    if (lfb_map.size) { __dpmi_free_physical_address_mapping(&lfb_map); lfb_map.size = 0; }
}
static void on_fault(int sig) {
    restore_screen();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void platform_init() {
    t_start = uclock();
    __djgpp_set_ctrl_c(0);                               // Ctrl+C is a key, not SIGINT
    _go32_want_ctrl_break(1);
    klogf("Felix BASIC for MS-DOS (DJGPP)\n");
    if (const char* e = getenv("FELIXFPS")) { int f = atoi(e); if (f >= 1 && f <= 200) dos_frame_ms = 1000 / f; }
    if (!vesa_init()) vga12_init();
    if (fb_indexed) pal_reset();
    video_ready = true;
    atexit(restore_screen);
    signal(SIGSEGV, on_fault); signal(SIGFPE, on_fault); signal(SIGILL, on_fault);
    signal(SIGABRT, on_fault);
    setvbuf(stdout, nullptr, _IONBF, 0);
    __FSEXT_set_function(1, con_hook);
    __FSEXT_set_function(2, con_hook);
    mouse_init();
    player_init(false);
    klogf("Sound: PC speaker\n");
    klogf("Memory: %lu KB free\n", (unsigned long)(heap_largest_free() >> 10));
}

int felix_basic_main(int argc, char** argv);   // ../main.cpp's main()

int main(int argc, char** argv) {
    platform_init();
    return felix_basic_main(argc, argv);
}
