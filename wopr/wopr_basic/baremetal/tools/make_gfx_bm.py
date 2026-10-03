#!/usr/bin/env python3
"""Generate basic_gfx_bm.cpp from ../basic_gfx_sdl.cpp.

The bare-metal display backend keeps everything in the SDL backend that
doesn't touch SDL or FreeType -- the palette, the pixel pages, every drawing
primitive, the text grid and scrollback, PRINT/LOCATE/INPUT -- verbatim, and
replaces the window, font, event pump and renderer with ones that draw into
the kernel's framebuffer with the 8x16 VGA font. Rerun after changing
basic_gfx_sdl.cpp (the Makefile does).
"""
import re, sys

src_path, out_path = sys.argv[1], sys.argv[2]
s = open(src_path).read()

def cut(a, b):
    """Text between markers a (inclusive) and b (exclusive)."""
    i = s.index(a); j = s.index(b, i)
    return i, j

def replace_between(text, a, b, new):
    i = text.index(a); j = text.index(b, i)
    return text[:i] + new + text[j:]

def sub(text, old, new, count=1):
    if old not in text:
        sys.exit("make_gfx_bm.py: can't find:\n" + old[:200])
    return text.replace(old, new, count)

# ---- the file's own description
i = s.index('/*'); j = s.index('*/', i) + 2
s = s[:i] + '''/*
 * basic_gfx_bm.cpp -- bare-metal implementation of basic_gfx.h and display.h.
 *
 * The SDL backend (basic_gfx_sdl.cpp) with the window, FreeType text and
 * event loop swapped for the kernel's framebuffer, the 8x16 VGA font and the
 * PC keyboard. Palette, pixel pages, drawing primitives, sprites, the text
 * grid, scrollback and line input are the SDL backend's code, unchanged.
 */''' + s[j:]

# ---- headers: no SDL/FreeType/font file; the kernel's framebuffer instead
s = sub(s, '#include "DejaVuMono.h"  // DEJAVU_REGULAR_FONT_B64 / _SIZE\n', '')
s = sub(s, '''#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
''', '''#include <SDL2/SDL.h>     // bare-metal stand-in: types, SDL_GetTicks, SDL_Delay
#include "font.h"         // 8x16 VGA font
#include "video.hpp"      // the kernel's framebuffer
''')
s = sub(s, '#include <mutex>\n', '')

# ---- SDL window/renderer globals -> display layout
s = replace_between(s, '''// ============================================================================
// SDL globals''', '''// ============================================================================
// Frame rate limiting''', '''// ============================================================================
// Screen layout (bare metal): where the text grid / graphics page land on
// the framebuffer, scaled to fill it as a monitor of the day would have.
// ============================================================================
static bool          s_needs_render = true;
static Uint32        s_last_dirty = 0;
static int           s_disp_x = 0, s_disp_y = 0, s_disp_w = 640, s_disp_h = 400;
static std::vector<int> s_xmap;                 // screen column -> graphics page column
static bool          s_layout_changed = true;    // clear the borders on the next render

''')
s = sub(s, 'static SDL_Texture         *s_gfx_tex = nullptr;\n', '')
s = sub(s, '''static int                  s_init_win_w = 800;  // Remember initial window size
static int                  s_init_win_h = 600;
''', '')

# ---- sprites in a std::map: its tree code is ours (rbtree.cpp), while
# std::unordered_map would pull in libstdc++'s prebuilt hashtable objects
s = sub(s, 'static std::unordered_map<int, Sprite> s_sprites;', 'static std::map<int, Sprite> s_sprites;')
s = sub(s, '#include <unordered_map>\n', '#include <map>\n')

# ---- scrollback: 5000 lines x 220 columns is 3 MB; keep 200 on bare metal
s = sub(s, '#define SCROLLBACK_LINES 5000', '#define SCROLLBACK_LINES 200')

# ---- FreeType, keyboard queue, init, pump, render -> bare-metal versions
NEW_CORE = r'''// ============================================================================
// Font metrics: the on-screen cell size (the 8x16 font scaled to fit)
// ============================================================================
static int s_cell_w = 8;
static int s_cell_h = 16;

void gfx_maybe_mark_dirty() {
    Uint32 now = SDL_GetTicks();
    if (now - s_last_dirty >= 16) {   // ~60 FPS
        s_needs_render = true;
        s_last_dirty = now;
    }
}

// Work out where things go on the framebuffer for the current mode. Every
// mode fills the largest 4:3 area of the screen, the shape of the monitors
// these modes were made for (an EGA's 640x350 filled one, as 320x200 did,
// and as the SDL window shows them; CIRCLE's aspect ratio keeps circles
// round). Text mode: 80 (or 40) x 25 cells of the 8x16 font (40 columns
// get double-width characters, as on the PC and the Apple II), at a
// whole-number scale when that comes close to filling it, so the font
// stays crisp. Graphics: the page stretched to fit, with the 80x25 text
// grid over it.
static void layout() {
    int fw = (int)fb_width(), fh = (int)fb_height();
    int dh = std::min(fh, fw * 3 / 4);
    int dw = std::min(fw, dh * 4 / 3);
    if (!s_gfx_active) {
        s_text_rows = TEXT_ROWS_DEF;
        int bw = 640 / s_text_cols, bh = 16, th = s_text_rows * bh;
        int k = std::max(1, std::min(dw / 640, dh / th));
        if (640 * k * 5 >= dw * 4 && th * k * 5 >= dh * 4) {
            s_cell_w = bw * k; s_cell_h = bh * k;            // crisp
        } else {
            s_cell_w = std::max(bw, dw / s_text_cols);       // stretched to fit
            s_cell_h = std::max(bh, dh / s_text_rows);
        }
        s_disp_w = s_cell_w * s_text_cols; s_disp_h = s_cell_h * s_text_rows;
    } else {
        int gw = std::max(1, s_gfx_w);
        s_disp_w = std::max(1, dw); s_disp_h = std::max(1, dh);
        s_xmap.resize((size_t)s_disp_w);
        for (int x = 0; x < s_disp_w; x++) s_xmap[(size_t)x] = (int)((long long)x * gw / s_disp_w);
        s_text_cols = TEXT_COLS_DEF; s_text_rows = TEXT_ROWS_DEF;
        s_cell_w = std::max(1, s_disp_w / s_text_cols);
        s_cell_h = std::max(1, s_disp_h / s_text_rows);
    }
    s_disp_x = std::max(0, (fw - s_disp_w) / 2);
    s_disp_y = std::max(0, (fh - s_disp_h) / 2);
    s_layout_changed = true;
    s_needs_render = true;
}

// ============================================================================
// Keyboard queue (filled by the kernel's keyboard driver)
// ============================================================================
static int s_keys[256];
static unsigned s_key_head, s_key_tail;
void gfx_bm_key(int c) {
    if (s_key_head - s_key_tail < 256) s_keys[s_key_head++ % 256] = c;
}
static int key_pop() {
    if (s_key_head == s_key_tail) return -1;
    return s_keys[s_key_tail++ % 256];
}

// ============================================================================
// Init / shutdown / event pump
// ============================================================================
static bool s_quit = false;   // never set: there's no window to close

bool gfx_sdl_init(const char * /*title*/, int /*w*/, int /*h*/) {
    // Called each time BASIC starts (also after SYSTEM): start from a plain
    // 80-column text screen, as at power-on.
    s_gfx_active = false; s_truecolor = false;
    for (int i = 0; i < GFX_MAX_PAGES; i++) std::vector<Uint32>().swap(s_pages[i]);
    s_gfx_w = s_gfx_h = 0; s_apage = s_vpage = 0;
    s_text_cols = TEXT_COLS_DEF; s_text_rows = TEXT_ROWS_DEF;
    s_cur_fg = 7; s_cur_bg = 0; s_cur_row = s_cur_col = 0;
    gfx_palette_reset();
    for (auto &row : s_grid)
        for (auto &c : row) c = {' ', 7, 0};
    layout();
    return true;
}

void gfx_sdl_shutdown() {}

BASIC_NS_BEGIN
void gfx_sdl_set_fullscreen(bool) {}             // always full screen
void gfx_sdl_toggle_fullscreen() {}
void gfx_palette_reset_pub() { gfx_palette_reset(); }
BASIC_NS_END

// ============================================================================
// The mouse (kernel.cpp: PS/2 or USB, as relative motion). The pointer is
// kept in screen pixels inside the picture; BASIC sees it as a fraction of
// the picture (mouse_report). At the prompt (and in any SCREEN 0 program
// that doesn't use the mouse) the left button selects text, Ctrl+C copies
// it and Ctrl+V types it back; the wheel scrolls through the history.
// ============================================================================
static int  s_mpx, s_mpy;                 // pointer, screen pixels
static bool s_mplaced;                    // put in the middle of the picture yet
static int  s_mbuttons;
static bool s_mouse_seen;                 // a mouse has reported
static bool s_pointer_drawn;               // the last render drew the pointer
static std::string s_clip;                // what Ctrl+C copied

static void mouse_place() {
    if (!s_mplaced) { s_mpx = s_disp_x + s_disp_w / 2; s_mpy = s_disp_y + s_disp_h / 2; s_mplaced = true; }
    s_mpx = std::max(s_disp_x, std::min(s_disp_x + s_disp_w - 1, s_mpx));
    s_mpy = std::max(s_disp_y, std::min(s_disp_y + s_disp_h - 1, s_mpy));
}
static int mouse_fx() { return (int)((long long)(s_mpx - s_disp_x) * 65536 / std::max(1, s_disp_w)); }
static int mouse_fy() { return (int)((long long)(s_mpy - s_disp_y) * 65536 / std::max(1, s_disp_h)); }
static void mouse_backend_warp(int fx, int fy) {
    mouse_place();
    s_mpx = s_disp_x + (int)((long long)fx * s_disp_w >> 16);
    s_mpy = s_disp_y + (int)((long long)fy * s_disp_h >> 16);
    mouse_place();
}
static void mouse_backend_cursor() { s_needs_render = true; }

// Show the pointer? Once a mouse has reported, always, unless a program
// using the mouse has hidden it (INT 33h function 2, _MOUSEHIDE).
static bool pointer_visible() {
    return s_mouse_seen && mouse_cursor_wanted();
}

static void scroll_lines(int n) {
    int lines_in_scrollback = std::min(s_scrollback_count, SCROLLBACK_LINES);
    s_display_offset = std::max(0, std::min(s_display_offset + n, lines_in_scrollback - s_text_rows));
    s_needs_render = true;
}

void gfx_bm_mouse(int dx, int dy, int buttons, int wheel) {
    mouse_place();
    int k = std::max(1, s_disp_w / 640);                  // the same hand movement on any screen
    s_mpx += dx * k; s_mpy += dy * k;
    mouse_place();
    if (dx || dy || buttons != s_mbuttons || wheel) {
        s_mouse_seen = true;
        s_needs_render = true;
    }
    int pressed = buttons & ~s_mbuttons, released = s_mbuttons & ~buttons;
    s_mbuttons = buttons;
    mouse_report(mouse_fx(), mouse_fy(), buttons, s_mouse_owned ? wheel : 0);
    if (s_mouse_owned) return;
    if (wheel) scroll_lines(wheel * 3);
    if (s_gfx_active) return;
    int col = std::max(0, std::min(s_text_cols - 1, (s_mpx - s_disp_x) / std::max(1, s_cell_w)));
    int row = std::max(0, std::min(s_text_rows - 1, (s_mpy - s_disp_y) / std::max(1, s_cell_h)));
    if (pressed & 1) sel_press(row, col);
    else if (buttons & 1) sel_drag(row, col);
    if (released & 1) sel_release();
}

bool gfx_bm_copy() {
    std::string t;
    if (!sel_take(t)) return false;
    s_clip = t;
    g_bm_selection = false;
    return true;
}
void gfx_bm_paste() {
    for (size_t i = 0; i < s_clip.size(); i++) {
        char c = s_clip[i];
        if (c == '\r' && i + 1 < s_clip.size() && s_clip[i + 1] == '\n') continue;
        gfx_bm_key(c == '\n' || c == '\r' ? '\r' : (unsigned char)c);
    }
}

// Poll the keyboard and mouse (kernel.cpp turns scancodes into gfx_bm_key()
// calls, mouse packets into gfx_bm_mouse()). Shift+PageUp/PageDown scroll
// through the history, as in the SDL build.
bool gfx_sdl_pump() {
    platform_poll_input();
    g_bm_selection = s_selecting && !s_selection_text.empty();
    if (s_pointer_drawn != pointer_visible()) s_needs_render = true;   // it timed out
    return true;
}
void gfx_bm_scroll(int pages) {
    int lines_in_scrollback = std::min(s_scrollback_count, SCROLLBACK_LINES);
    if (pages > 0)
        s_display_offset = std::max(0, std::min(s_display_offset + s_text_rows, lines_in_scrollback - s_text_rows));
    else
        s_display_offset = std::max(0, s_display_offset - s_text_rows);
    s_needs_render = true;
}

// ============================================================================
// Rendering into the kernel's back buffer
// ============================================================================
// CP437 line drawing (179-218) as up/down/left/right strokes: 0 none,
// 1 single, 2 double. The 8x16 font only has ASCII; these and the block
// and shade characters (176-178, 219-223) are drawn from this table.
static const uint8_t s_box[40][4] = {
    {1,1,0,0},{1,1,1,0},{1,1,2,0},{2,2,1,0},{0,2,1,0},{0,1,2,0},{2,2,2,0},{2,2,0,0},  // 179-186
    {0,2,2,0},{2,0,2,0},{2,0,1,0},{1,0,2,0},{0,1,1,0},{1,0,0,1},{1,0,1,1},{0,1,1,1},  // 187-194
    {1,1,0,1},{0,0,1,1},{1,1,1,1},{1,1,0,2},{2,2,0,1},{2,0,0,2},{0,2,0,2},{2,0,2,2},  // 195-202
    {0,2,2,2},{2,2,0,2},{0,0,2,2},{2,2,2,2},{1,0,2,2},{2,0,1,1},{0,1,2,2},{0,2,1,1},  // 203-210
    {2,0,0,1},{1,0,0,2},{0,1,0,2},{0,2,0,1},{2,2,1,1},{1,1,2,2},{1,0,1,0},{0,1,0,1},  // 211-218
};
// Is pixel (x, y) of an 8x16 cell set, for character ch?
static bool glyph_px(unsigned char ch, int x, int y) {
    if (ch >= 32 && ch < 127) return (font8x16[ch - 32][y] >> (7 - x)) & 1;
    if (ch == 219) return true;                                  // full block
    if (ch == 220) return y >= 8;                                // lower half
    if (ch == 223) return y < 8;                                 // upper half
    if (ch == 221) return x < 4;                                 // left half
    if (ch == 222) return x >= 4;                                // right half
    if (ch >= 176 && ch <= 178) {                                // shades
        int k = (x + (y & 1) * 2) & 3;
        return ch == 176 ? (y % 2 == 0 && x % 4 == 0) || (y % 2 == 1 && x % 4 == 2)
             : ch == 177 ? ((x + y) & 1) : k != 3;
    }
    if (ch >= 179 && ch <= 218) {
        const uint8_t* b = s_box[ch - 179];
        // single strokes on row 7 / column 3; double on rows 6,8 / columns 2,4
        bool hs = (b[2] | b[3]) != 0, vs = (b[0] | b[1]) != 0;
        int hw = std::max(b[2], b[3]), vw = std::max(b[0], b[1]);
        bool on = false;
        if (hs) {
            bool row = hw == 2 ? (y == 6 || y == 8) : y == 7;
            int lo = b[2] ? 0 : (vw == 2 ? 2 : 3), hi = b[3] ? 7 : (vw == 2 ? 4 : 3);
            if (row && x >= lo && x <= hi) on = true;
        }
        if (vs) {
            bool col = vw == 2 ? (x == 2 || x == 4) : x == 3;
            int lo = b[0] ? 0 : (hw == 2 ? 6 : 7), hi = b[1] ? 15 : (hw == 2 ? 8 : 7);
            if (col && y >= lo && y <= hi) on = true;
        }
        return on;
    }
    if (ch == 127 || ch < 32) return false;
    return y >= 3 && y <= 12 && x >= 1 && x <= 6 && (x == 1 || x == 6 || y == 3 || y == 12);   // unknown: a box
}

// Which font row (of 16) / column (of 8) each pixel of a cell shows, or -1
// for padding. A cell close to a whole multiple of the font (8x16 in a
// 8x19 cell, say: 640x480 over 25 rows) gets the font at that multiple,
// crisp, centred with a little padding; a cell well between multiples
// (13 wide: 1.6 times) has the font stretched to it. Cells of 14 or 15
// lines (EGA's 640x350) drop the font's blank top/bottom rows.
static int s_glyph_x[256], s_glyph_y[256];
// The same with the padding filled from the nearest font row / column, for
// the box-drawing and block characters (176-223), which have to meet the
// next cell's without a gap (Nibbles' walls, a text window's frame).
static int s_glyph_xe[256], s_glyph_ye[256];
static int s_glyph_cw = -1, s_glyph_ch = -1;
static void glyph_axis(int* map, int cell, int font) {
    cell = std::min(cell, 256);
    int k = cell / font;
    if (k >= 1 && cell - k * font < font / 2) {          // crisp, padded
        int pad = (cell - k * font) / 2;
        for (int i = 0; i < cell; i++) {
            int f = (i - pad) / k;
            map[i] = (i >= pad && f < font) ? f : -1;
        }
    } else if (font == 16 && (cell == 14 || cell == 15)) {  // EGA: crop
        for (int i = 0; i < cell; i++) map[i] = i + (16 - cell + 1) / 2;
    } else {                                                 // stretched
        for (int i = 0; i < cell; i++) map[i] = i * font / cell;
    }
}
static void glyph_maps() {
    if (s_glyph_cw == s_cell_w && s_glyph_ch == s_cell_h) return;
    glyph_axis(s_glyph_x, s_cell_w, 8);
    glyph_axis(s_glyph_y, s_cell_h, 16);
    for (int pass = 0; pass < 2; pass++) {
        const int* m = pass ? s_glyph_y : s_glyph_x;
        int* e = pass ? s_glyph_ye : s_glyph_xe;
        int n = std::min(pass ? s_cell_h : s_cell_w, 256), last = pass ? 15 : 7;
        int first = -1;
        for (int i = 0; i < n && first < 0; i++) if (m[i] >= 0) first = m[i];
        int cur = first < 0 ? 0 : first;
        for (int i = 0; i < n; i++) {
            if (m[i] >= 0) cur = m[i];
            e[i] = m[i] >= 0 ? m[i] : (i < n / 2 ? (first < 0 ? 0 : first) : (cur > last ? last : cur));
        }
    }
    s_glyph_cw = s_cell_w; s_glyph_ch = s_cell_h;
}

// Draw text row `row`'s cells into a strip whose first line is screen line y0.
static void render_text_row(uint32_t* strip, int row, int y0, int lines) {
    uint32_t w = fb_width();
    glyph_maps();
    for (int col = 0; col < s_text_cols; col++) {
        const Cell cell = scrollback_get_cell(row, col);
        unsigned char ch = (unsigned char)cell.ch;
        bool blank = ch == ' ' || ch == 0;
        bool joins = ch >= 176 && ch <= 223;
        const int* gxm = joins ? s_glyph_xe : s_glyph_x;
        const int* gym = joins ? s_glyph_ye : s_glyph_y;
        // Text mode paints every cell's background; in graphics mode only
        // cells with something in them, so the picture shows through.
        if (s_gfx_active && blank) continue;
        Uint32 fg = s_pal[cell.fg & 15] & 0xFFFFFF, bg = s_pal[cell.bg & 15] & 0xFFFFFF;
        if (is_cell_selected(row, col)) {                // light blue over it, as in the SDL build
            auto tint = [](Uint32 c) {
                Uint32 r = ((c >> 16 & 255) * 155 + 100 * 100) / 255;
                Uint32 g = ((c >> 8 & 255) * 155 + 150 * 100) / 255;
                Uint32 b = ((c & 255) * 155 + 255 * 100) / 255;
                return r << 16 | g << 8 | b;
            };
            fg = tint(fg); bg = tint(bg);
        }
        int x0 = s_disp_x + col * s_cell_w;
        int cy0 = s_disp_y + row * s_cell_h;
        int cw = std::min(s_cell_w, 256), chh = std::min(s_cell_h, 256);
        for (int y = 0; y < chh; y++) {
            int sy = cy0 + y - y0;
            if (sy < 0 || sy >= lines) continue;
            int gy = gym[y];
            Uint32* d = &strip[(size_t)sy * w + x0];
            for (int x = 0; x < cw; x++) {
                int gx = gxm[x];
                d[x] = (!blank && gy >= 0 && gx >= 0 && glyph_px(ch, gx, gy)) ? fg : bg;
            }
        }
    }
}

// Compose and copy out the screen lines y0 .. y0+lines-1.
static void render_strip(int y0, int lines) {
    uint32_t w = fb_width();
    uint32_t* strip = video_band(y0, lines);
    if (!strip) return;
    for (size_t i = 0; i < (size_t)w * lines; i++) strip[i] = 0;
    // 1. The visible graphics page, scaled to the display area
    if (s_gfx_active) {
        const std::vector<Uint32> &vpx = (s_vpage < GFX_MAX_PAGES && !s_pages[s_vpage].empty())
                                          ? s_pages[s_vpage] : s_pages[0];
        if (!vpx.empty() && (int)s_xmap.size() == s_disp_w && s_gfx_h > 0) {
            const int* xm = s_xmap.data();
            for (int r = 0; r < lines; r++) {
                int y = y0 + r - s_disp_y;
                if (y < 0 || y >= s_disp_h) continue;
                int gy = (int)((long long)y * s_gfx_h / s_disp_h);
                const Uint32* src = &vpx[(size_t)gy * s_gfx_w];
                Uint32* d = &strip[(size_t)r * w + s_disp_x];
                for (int x = 0; x < s_disp_w; x++) d[x] = src[xm[x]] & 0xFFFFFF;
            }
        }
    }
    // 2. The text grid (always, even over graphics)
    int r0 = std::max(0, (y0 - s_disp_y) / s_cell_h);
    for (int row = r0; row < s_text_rows && s_disp_y + row * s_cell_h < y0 + lines; row++)
        render_text_row(strip, row, y0, lines);
    // 3. The cursor: an underline, two scan lines of the font
    if (s_cursor_vis && s_display_offset == 0 && s_cur_row < s_text_rows && s_cur_col < s_text_cols) {
        int k = std::max(1, s_cell_h / 16);
        int x0 = s_disp_x + s_cur_col * s_cell_w;
        int cy = s_disp_y + s_cur_row * s_cell_h + s_cell_h - 3 * k;
        Uint32 c = s_pal[s_cur_fg & 15] & 0xFFFFFF;
        for (int y = cy; y < cy + 2 * k; y++)
            if (y >= y0 && y < y0 + lines)
                for (int x = x0; x < x0 + s_cell_w; x++) strip[(size_t)(y - y0) * w + x] = c;
    }
    // 4. The mouse pointer: an arrow, black outline and white inside, its
    // tip on the pointer's pixel, scaled up with the screen
    if (s_pointer_drawn) {
        static const char *const arrow[] = {
            "X...........", "XX..........", "XOX.........", "XOOX........", "XOOOX.......",
            "XOOOOX......", "XOOOOOX.....", "XOOOOOOX....", "XOOOOOOOX...", "XOOOOOOOOX..",
            "XOOOOOOOOOX.", "XOOOOOOXXXXX", "XOOOXOOX....", "XOOXXOOX....", "XOX..XOOX...",
            "XX...XOOX...", "X.....XOOX..", "......XOOX..", ".......XX...",
        };
        int k = std::max(1, (int)fb_height() / 480);
        for (int r = 0; r < 19 * k; r++) {
            int y = s_mpy + r;
            if (y < y0 || y >= y0 + lines || y >= (int)fb_height()) continue;
            for (int c = 0; c < 12 * k; c++) {
                int x = s_mpx + c;
                char a = arrow[r / k][c / k];
                if (a == '.' || x >= (int)w) continue;
                strip[(size_t)(y - y0) * w + x] = a == 'X' ? 0x000000 : 0xFFFFFF;
            }
        }
    }
    video_band_done();
}

void gfx_sdl_render() {
    if (!s_needs_render) return;
    s_needs_render = false;
    s_pointer_drawn = pointer_visible();
    if (s_pointer_drawn) mouse_place();
    int h = (int)fb_height();
    if (s_layout_changed) {                                      // new mode: repaint the borders too
        video_invalidate();
        for (int y = 0; y < s_disp_y; y += 16) render_strip(y, std::min(16, s_disp_y - y));
        for (int y = s_disp_y + s_disp_h; y < h; y += 16) render_strip(y, std::min(16, h - y));
        s_layout_changed = false;
    }
    // One strip per text row, then whatever is left of the picture below it.
    int y = s_disp_y, end = s_disp_y + s_disp_h;
    for (int row = 0; row < s_text_rows && y < end; row++, y += s_cell_h)
        render_strip(y, std::min(s_cell_h, end - y));
    for (; y < end; y += 16) render_strip(y, std::min(16, end - y));
    // The pointer can hang over the border below the picture; those rows
    // too (only the ones that changed are copied to the screen)
    int below = std::min(h - end, 19 * std::max(1, (int)fb_height() / 480));
    if (below > 0) render_strip(end, below);
}

'''
s = replace_between(s, '''// ============================================================================
// FreeType / glyph cache''', '''void gfx_sdl_mark_dirty()''', NEW_CORE)

# ---- SCREEN mode changes: no window or texture to resize, just re-lay out
s = replace_between(s, '''    s_gfx_w = w; s_gfx_h = h;
    // Size window to fit display''', '''    for (int i = 0; i < GFX_MAX_PAGES; i++) s_pages[i].assign''', '''    s_gfx_w = w; s_gfx_h = h;
''')
s = sub(s, '''    if (s_gfx_tex) SDL_DestroyTexture(s_gfx_tex);
    s_gfx_tex = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureBlendMode(s_gfx_tex, SDL_BLENDMODE_NONE);
    s_gfx_active = true;
    s_text_cols = TEXT_COLS_DEF; s_text_rows = TEXT_ROWS_DEF;
    ft_set_size_for_window();''', '''    s_gfx_active = true;
    s_text_cols = TEXT_COLS_DEF; s_text_rows = TEXT_ROWS_DEF;
    layout();''')
s = sub(s, '''        if (s_gfx_tex) { SDL_DestroyTexture(s_gfx_tex); s_gfx_tex = nullptr; }
''', '')
s = sub(s, '''        // Restore window to initial size
        if (s_window) SDL_SetWindowSize(s_window, s_init_win_w, s_init_win_h);
        s_win_w = s_init_win_w;
        s_win_h = s_init_win_h;
        ft_set_size_for_window();''', '''        s_text_cols = TEXT_COLS_DEF;
        layout();''')
s = replace_between(s, '''    // All CGA/EGA modes should display with 4:3 aspect ratio''', '''        // Allocate all pages''', '''    if (dims_changed) {
''')
s = sub(s, '''        if (s_gfx_tex) SDL_DestroyTexture(s_gfx_tex);
        s_gfx_tex = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, gw, gh);
        SDL_SetTextureBlendMode(s_gfx_tex, SDL_BLENDMODE_BLEND);
''', '')
s = sub(s, '''    s_text_rows = TEXT_ROWS_DEF;
    ft_set_size_for_window();
    s_needs_render = true;
}''', '''    s_text_rows = TEXT_ROWS_DEF;
    layout();
}''')

# ---- pixel pages: allocate page 0 now and the others when SCREEN first
# selects them (four 640x480 pages would be 5 MB; most programs use one)
# A page that doesn't fit gives "Out of memory": a mode that doesn't fit
# leaves the screen in text mode, a page that doesn't fit isn't selected.
s = sub(s, '''void gfx_screen_tc(int w, int h) {''', '''extern "C" size_t heap_largest_free(void);
BASIC_NS_BEGIN int basic_stderr(const char *fmt, ...); BASIC_NS_END
static bool page_fits(int w, int h) {
    return heap_largest_free() >= (size_t)w * (size_t)h * sizeof(Uint32) + 4096;
}
static void free_pages() {
    for (int i = 0; i < GFX_MAX_PAGES; i++) std::vector<Uint32>().swap(s_pages[i]);
}
static void screen_out_of_memory() {
    free_pages();
    s_gfx_active = false;
    s_gfx_w = s_gfx_h = 0;
    s_apage = s_vpage = 0;
    s_text_cols = TEXT_COLS_DEF; s_text_rows = TEXT_ROWS_DEF;
    layout();
    s_needs_render = true;
    BASIC_NS::basic_stderr("Out of memory\\n");
}

void gfx_screen_tc(int w, int h) {''')
s = sub(s, '''        // Allocate all pages
        for (int i = 0; i < GFX_MAX_PAGES; i++)
            s_pages[i].assign((size_t)(gw * gh), color_to_pixel(0));''', '''        // Page 0 now; the others when first selected (below). The old
        // pages go first, so the new one can have their memory.
        free_pages();
        if (!page_fits(gw, gh)) { screen_out_of_memory(); return; }
        s_pages[0].assign((size_t)(gw * gh), color_to_pixel(0));''')
s = sub(s, '''    if (vpage >= 0 && vpage < GFX_MAX_PAGES) s_vpage = vpage;
''', '''    if (vpage >= 0 && vpage < GFX_MAX_PAGES) s_vpage = vpage;
    for (int *pg : {&s_apage, &s_vpage}) {
        if (s_pages[*pg].size() == (size_t)(gw * gh)) continue;
        if (page_fits(gw, gh)) s_pages[*pg].assign((size_t)(gw * gh), color_to_pixel(0));
        else { *pg = 0; BASIC_NS::basic_stderr("Out of memory\\n"); }
    }
''')
s = sub(s, '''    for (int i = 0; i < GFX_MAX_PAGES; i++) s_pages[i].assign((size_t)(w * h), 0xFF000000u);''',
           '''    free_pages();
    if (!page_fits(w, h)) { s_truecolor = false; screen_out_of_memory(); return; }
    s_pages[0].assign((size_t)(w * h), 0xFF000000u);
    s_apage = s_vpage = 0;''')

s = sub(s, '''        for (int i = 0; i < GFX_MAX_PAGES; i++) s_pages[i].clear();''',
           '''        for (int i = 0; i < GFX_MAX_PAGES; i++) std::vector<Uint32>().swap(s_pages[i]);   // give the memory back''')

# ---- WIDTH 40/80 changes the cell size in text mode
s = sub(s, '''    s_text_cols = (cols <= 40) ? 40 : 80;
    s_needs_render = true;
    if (!s_gfx_active) ft_set_size_for_window();''', '''    s_text_cols = (cols <= 40) ? 40 : 80;
    layout();''')

header = ('// GENERATED by tools/make_gfx_bm.py from ../basic_gfx_sdl.cpp -- edit that\n'
          '// script (or the SDL backend), not this file.\n\n')
leftover = [w for w in ('SDL_Window', 'SDL_Renderer', 'SDL_Texture', 'FT_', 's_win_w', 'ft_set_size', 'TEXT_PAD', 's_gfx_tex')
            if w in s]
if leftover:
    sys.exit('make_gfx_bm.py: SDL/FreeType leftovers: ' + ', '.join(leftover))
open(out_path, 'w').write(header + s)
