/*
 * basic_gfx_sdl.cpp — SDL2 implementation of basic_gfx.h
 *
 * Owns:
 *   • A 32-bit ARGB pixel buffer sized to the active SCREEN mode
 *   • An SDL_Texture (streaming) uploaded each frame
 *   • The 16-colour CGA palette, overridable via gfx_palette()
 *   • Sprite store for GET / PUT
 *   • FreeType text-mode glyph rendering (DejaVu embedded font)
 *   • The SDL window, renderer, and event loop
 *
 * The display_* functions (display.h) are also implemented here,
 * replacing display_ansi.cpp entirely when USE_SDL_WINDOW is defined.
 *
 * Build:
 *   Compile this file instead of display_ansi.cpp, plus basic_gfx_sdl.cpp.
 *   Add -DUSE_SDL_WINDOW -DHAVE_SDL to the compiler flags.
 */

#ifdef USE_SDL_WINDOW

#include "basic_gfx.h"
#include "basic_gfx_sdl.h"
#include "display.h"
#include "basic.h"       // g_break, BASIC_NS
#include "DejaVuMono.h"  // DEJAVU_REGULAR_FONT_B64 / _SIZE

#ifdef WOPR
#include "wopr.h"
extern WoprState g_wopr;
extern void wopr_basic_cls(void);
extern void wopr_basic_locate(int row, int col);
extern void wopr_basic_color(int fg, int bg);
#endif

#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <string>

#include "basic_ns.h"

// ============================================================================
// CGA palette  (ARGB)
// ============================================================================
static Uint32 s_pal[16] = {
    0xFF000000, // 0  Black
    0xFF0000AA, // 1  Blue
    0xFF00AA00, // 2  Green
    0xFF00AAAA, // 3  Cyan
    0xFFAA0000, // 4  Red
    0xFFAA00AA, // 5  Magenta
    0xFFAA5500, // 6  Brown
    0xFFAAAAAA, // 7  Light Grey
    0xFF555555, // 8  Dark Grey
    0xFF5555FF, // 9  Light Blue
    0xFF55FF55, // 10 Light Green
    0xFF55FFFF, // 11 Light Cyan
    0xFFFF5555, // 12 Light Red
    0xFFFF55FF, // 13 Light Magenta
    0xFFFFFF55, // 14 Yellow
    0xFFFFFFFF, // 15 White
};

static inline Uint8 pal_r(int i) { i&=15; return (s_pal[i]>>16)&0xFF; }
static inline Uint8 pal_g(int i) { i&=15; return (s_pal[i]>> 8)&0xFF; }
static inline Uint8 pal_b(int i) { i&=15; return (s_pal[i]    )&0xFF; }
static int s_font_gen;
static int s_pal_gen;   // bumped whenever any palette slot changes

static const Uint32 s_pal_default[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

static void gfx_palette_reset() {
    memcpy(s_pal, s_pal_default, sizeof(s_pal));
    s_pal_gen++;
}

// ============================================================================
// SDL globals
// ============================================================================
static SDL_Window   *s_window   = nullptr;
static SDL_Renderer *s_renderer = nullptr;
static bool          s_needs_render = true;
static int           s_win_w = 800, s_win_h = 600;
static Uint32        s_last_dirty = 0;

// ============================================================================
// Frame rate limiting (60 FPS)
// ============================================================================
static Uint32        s_frame_start_time = 0;
static const Uint32  FRAME_TIME_MS = 16;  /* ~60 FPS (1000/60 = 16.67 ms) */

// ============================================================================
// Graphics surface
// ============================================================================
static SDL_Texture         *s_gfx_tex = nullptr;
static std::vector<Uint32>  s_pixels;          // ARGB, gfx_w × gfx_h
static int                  s_gfx_w = 0, s_gfx_h = 0;
static bool                 s_gfx_active = false;
static int                  s_init_win_w = 800;  // Remember initial window size
static int                  s_init_win_h = 600;
static bool                 s_truecolor  = false;  // true when SCREEN _NEWIMAGE(...,32)

// ============================================================================
// Page support (SCREEN mode, colorswitch, apage, vpage)
// Up to 4 pages; apage = active draw page, vpage = visible display page.
// ============================================================================
static const int GFX_MAX_PAGES = 4;
static std::vector<Uint32> s_pages[GFX_MAX_PAGES];  // per-page pixel buffers
static int s_apage = 0;   // active (draw) page
static int s_vpage = 0;   // visible (display) page

// ============================================================================
// Sprite store
// ============================================================================
struct Sprite { int w, h; std::vector<Uint32> px; int bg_color = 0; };
static std::unordered_map<int, Sprite> s_sprites;

void gfx_sprites_clear(void) { s_sprites.clear(); }

// ============================================================================
// Text grid — static max-size buffer; active area is s_text_cols × s_text_rows
// ============================================================================
#define TEXT_ROWS_MAX  60
#define TEXT_COLS_MAX  220
#define TEXT_ROWS_DEF  25    // default (graphics mode / initial)
#define TEXT_COLS_DEF  80
#define SCROLLBACK_LINES 5000

struct Cell { char ch; Uint8 fg, bg; };
static Cell s_grid[TEXT_ROWS_MAX][TEXT_COLS_MAX];
static int  s_text_cols  = TEXT_COLS_DEF;
static int  s_text_rows  = TEXT_ROWS_DEF;
static int  s_text_lock_rows = 0;   // SCREEN 0 after WIDTH: a fixed cols x rows grid (0: fit the window)
static int  s_cur_row    = 0, s_cur_col = 0;
static int  s_cur_fg     = 7, s_cur_bg  = 0;
static bool s_cursor_vis = false;

// ============================================================================
// Scrollback history buffer
// ============================================================================
static std::vector<std::vector<Cell>> s_scrollback;  // circular buffer of history lines
static int  s_scrollback_head = 0;                   // write position (next line to overwrite)
static int  s_scrollback_count = 0;                  // total lines written (monotonic)
static int  s_display_offset = 0;                    // view offset from bottom (0 = show latest)

// ============================================================================
// Text selection for copy/paste
// ============================================================================
static bool s_selecting  = false;      // mouse drag in progress
static int  s_sel_start_row = 0, s_sel_start_col = 0;  // selection anchor
static int  s_sel_end_row = 0, s_sel_end_col = 0;      // selection end
static std::string s_selection_text = "";              // cached selection buffer
static bool s_drag_occurred = false;   // track if mouse actually moved

// Update selection text from grid
static void update_selection_text() {
    s_selection_text.clear();
    int r1 = std::min(s_sel_start_row, s_sel_end_row);
    int r2 = std::max(s_sel_start_row, s_sel_end_row);
    int c1 = (s_sel_start_row == s_sel_end_row) ? std::min(s_sel_start_col, s_sel_end_col) : (s_sel_start_row < s_sel_end_row ? s_sel_start_col : s_sel_end_col);
    int c2 = (s_sel_start_row == s_sel_end_row) ? std::max(s_sel_start_col, s_sel_end_col) : (s_sel_start_row < s_sel_end_row ? s_sel_end_col : s_sel_start_col);
    
    for (int r = r1; r <= r2; r++) {
        int col_start = (r == r1) ? c1 : 0;
        int col_end = (r == r2) ? c2 : s_text_cols - 1;
        for (int c = col_start; c <= col_end && c < s_text_cols; c++) {
            s_selection_text += s_grid[r][c].ch;
        }
        if (r < r2) s_selection_text += '\n';
    }
}

// Check if a cell is within selection
static bool is_cell_selected(int row, int col) {
    if (!s_selecting) return false;
    int r1 = std::min(s_sel_start_row, s_sel_end_row);
    int r2 = std::max(s_sel_start_row, s_sel_end_row);
    if (row < r1 || row > r2) return false;
    if (row == r1 && row == r2) 
        return col >= std::min(s_sel_start_col, s_sel_end_col) && col <= std::max(s_sel_start_col, s_sel_end_col);
    if (row == r1) return col >= (s_sel_start_row < s_sel_end_row ? s_sel_start_col : s_sel_end_col);
    if (row == r2) return col <= (s_sel_start_row < s_sel_end_row ? s_sel_end_col : s_sel_start_col);
    return true;
}

// ============================================================================
// Scrollback history helpers
// ============================================================================
static void scrollback_init() {
    s_scrollback.clear();
    s_scrollback.resize(SCROLLBACK_LINES);
    for (int i = 0; i < SCROLLBACK_LINES; i++) {
        s_scrollback[i].resize(TEXT_COLS_MAX);
        for (int j = 0; j < TEXT_COLS_MAX; j++) {
            s_scrollback[i][j] = {' ', 7, 0};
        }
    }
    s_scrollback_head = 0;
    s_scrollback_count = 0;
    s_display_offset = 0;
}

// Push current s_grid[0] to scrollback, shift grid up
static void scrollback_push_line() {
    if (s_scrollback.empty()) scrollback_init();
    
    // Copy current top line to scrollback
    for (int col = 0; col < s_text_cols; col++) {
        s_scrollback[s_scrollback_head][col] = s_grid[0][col];
    }
    
    s_scrollback_head = (s_scrollback_head + 1) % SCROLLBACK_LINES;
    s_scrollback_count++;
    
    // Shift grid up
    for (int row = 0; row < s_text_rows - 1; row++) {
        for (int col = 0; col < s_text_cols; col++) {
            s_grid[row][col] = s_grid[row + 1][col];
        }
    }
    
    // Clear bottom line
    for (int col = 0; col < s_text_cols; col++) {
        s_grid[s_text_rows - 1][col] = {' ', s_cur_fg, s_cur_bg};
    }
}

// Get a cell from scrollback or current grid
static Cell scrollback_get_cell(int display_row, int col) {
    if (col >= s_text_cols) return {' ', 0, 0};
    
    int lines_in_scrollback = std::min(s_scrollback_count, SCROLLBACK_LINES);
    
    if (s_display_offset == 0) {
        // Showing current grid (latest lines, bottom of view)
        return s_grid[display_row][col];
    }
    
    // When scrolled, calculate which line to show
    // offset=0 shows lines [lines_in_scrollback - s_text_rows ... lines_in_scrollback - 1] (current grid)
    // offset=s_text_rows shows lines [lines_in_scrollback - 2*s_text_rows ... lines_in_scrollback - s_text_rows - 1]
    
    // The "bottom" of current view when scrolled
    int view_bottom_abs_line = lines_in_scrollback - s_display_offset - 1;
    int absolute_line = view_bottom_abs_line - (s_text_rows - 1 - display_row);
    
    // Handle bounds
    if (absolute_line < 0 || absolute_line >= lines_in_scrollback) {
        return {' ', 0, 0};  // Blank line
    }
    
    // Map absolute line to circular buffer index
    int oldest_idx = s_scrollback_count >= SCROLLBACK_LINES ? s_scrollback_head : 0;
    int sb_idx = (oldest_idx + absolute_line) % SCROLLBACK_LINES;
    
    return s_scrollback[sb_idx][col];
}

// ============================================================================
// FreeType / glyph cache
// ============================================================================
static FT_Library s_ft_lib  = nullptr;
static FT_Face    s_ft_face = nullptr;
static int        s_cell_w  = 8;
static int        s_cell_h  = 16;

#define TEXT_PAD  8   // px padding inside window on each side

struct Glyph {
    std::vector<Uint8> bm;  // grayscale w×h
    int bm_w, bm_h;
    int bearing_x, bearing_y;
};
static std::unordered_map<int, Glyph> s_glyph_cache;

// base64 decode (for embedded font)
static std::vector<Uint8> b64_decode(const char *src, size_t len) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<Uint8> out;
    out.reserve(len * 3 / 4);
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int8_t v = T[(uint8_t)src[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (uint8_t)v;
        if ((bits += 6) >= 8) { bits -= 8; out.push_back((acc >> bits) & 0xFF); }
    }
    return out;
}

// Kept alive for FT_New_Memory_Face
static std::vector<Uint8> s_font_data;

void gfx_maybe_mark_dirty() {
    Uint32 now = SDL_GetTicks();
    if (now - s_last_dirty >= 16) {   // ~60 FPS
        s_needs_render = true;
        s_last_dirty = now;
    }
}

static int s_font_size_override = 0;  // 0 = auto-fit, >0 = manual px height (SCREEN 0 only)

// Recompute cell size and (in SCREEN 0) grid dimensions from current window/font state.
// In graphics modes the grid stays at 80×25 regardless.
static void ft_set_size_for_window() {
    if (!s_ft_face) return;

    if (s_gfx_active) {
        // Graphics mode: size font to fit standard 80×25 into the window.
        // Keep grid locked at 80×25.
        s_text_cols = TEXT_COLS_DEF;
        s_text_rows = TEXT_ROWS_DEF;
        int avail_w = s_win_w - 2 * TEXT_PAD;
        int avail_h = s_win_h - 2 * TEXT_PAD;
        int lo = 6, hi = avail_h / TEXT_ROWS_DEF;
        if (hi < lo) hi = lo;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)mid);
            int adv = (int)(s_ft_face->size->metrics.max_advance >> 6);
            int ht  = (int)(s_ft_face->size->metrics.height >> 6);
            if (adv > 0 && ht > 0 && adv * TEXT_COLS_DEF <= avail_w && ht * TEXT_ROWS_DEF <= avail_h)
                lo = mid;
            else
                hi = mid - 1;
        }
        FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)lo);
    } else {
        // Text mode (SCREEN 0): font size drives how many cols/rows fit.
        int target;
        if (s_text_lock_rows > 0 && s_font_size_override <= 0) {
            // After WIDTH: keep the program's grid (QBasic's 80x25) and
            // scale the font to fill the window with it.
            int cols = s_text_cols <= 40 ? 40 : 80, rows = s_text_lock_rows;
            int avail_w = s_win_w - 2 * TEXT_PAD;
            int avail_h = s_win_h - 2 * TEXT_PAD;
            int lo = 6, hi = avail_h / rows;
            if (hi < lo) hi = lo;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)mid);
                int adv = (int)(s_ft_face->size->metrics.max_advance >> 6);
                int ht  = (int)(s_ft_face->size->metrics.height >> 6);
                if (adv > 0 && ht > 0 && adv * cols <= avail_w && ht * rows <= avail_h)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            target = lo;
        } else if (s_font_size_override > 0) {
            target = s_font_size_override;
        } else {
            // Auto: pick largest size that fits at least 80×25
            int avail_w = s_win_w - 2 * TEXT_PAD;
            int avail_h = s_win_h - 2 * TEXT_PAD;
            int lo = 6, hi = avail_h / TEXT_ROWS_DEF;
            if (hi < lo) hi = lo;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)mid);
                int adv = (int)(s_ft_face->size->metrics.max_advance >> 6);
                int ht  = (int)(s_ft_face->size->metrics.height >> 6);
                if (adv > 0 && ht > 0 && adv * TEXT_COLS_DEF <= avail_w && ht * TEXT_ROWS_DEF <= avail_h)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            target = lo;
        }
        FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)target);

        // Compute how many cols/rows actually fit at this font size, within limits
        int adv = (int)(s_ft_face->size->metrics.max_advance >> 6);
        int ht  = (int)(s_ft_face->size->metrics.height >> 6);
        if (s_text_lock_rows > 0 && s_font_size_override <= 0) {
            s_text_cols = s_text_cols <= 40 ? 40 : 80;
            s_text_rows = s_text_lock_rows;
        } else if (adv > 0 && ht > 0) {
            int cols = (s_win_w - 2 * TEXT_PAD) / adv;
            int rows = (s_win_h - 2 * TEXT_PAD) / ht;
            cols = std::max(40,  std::min(TEXT_COLS_MAX, cols));
            rows = std::max(10,  std::min(TEXT_ROWS_MAX, rows));
            s_text_cols = cols;
            s_text_rows = rows;
        }
    }

    int adv = (int)(s_ft_face->size->metrics.max_advance >> 6);
    int ht  = (int)(s_ft_face->size->metrics.height >> 6);
    if (adv > 0) s_cell_w = adv;
    if (ht  > 0) s_cell_h = ht;
    s_glyph_cache.clear();
    s_font_gen++;
}

static void ft_init() {
    if (!s_ft_lib) FT_Init_FreeType(&s_ft_lib);
    s_font_data = b64_decode(DEJAVU_REGULAR_FONT_B64, DEJAVU_REGULAR_FONT_B64_SIZE);
    FT_New_Memory_Face(s_ft_lib, s_font_data.data(), (FT_Long)s_font_data.size(), 0, &s_ft_face);
    ft_set_size_for_window();
    s_glyph_cache.clear();
}

bool gfx_sdl_load_font(const char *path) {
    if (!s_ft_lib) FT_Init_FreeType(&s_ft_lib);
    if (s_ft_face) { FT_Done_Face(s_ft_face); s_ft_face = nullptr; }
    if (FT_New_Face(s_ft_lib, path, 0, &s_ft_face) != 0) {
        ft_init();  // fallback to embedded
        return false;
    }
    ft_set_size_for_window();
    return true;
}

// CP437 (the IBM PC character set QBasic programs are written in) to
// Unicode, for looking glyphs up in the font: CHR$(219) is a full block,
// CHR$(196) a box line, CHR$(24) an up arrow, and so on.
static const unsigned short s_cp437[256] = {
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static const Glyph &glyph_get(int cp) {
    auto it = s_glyph_cache.find(cp);
    if (it != s_glyph_cache.end()) return it->second;

    Glyph g{}; g.bm_w = s_cell_w; g.bm_h = s_cell_h;
    g.bearing_x = 0; g.bearing_y = s_cell_h - 2;
    g.bm.assign((size_t)(s_cell_w * s_cell_h), 0);

    if (s_ft_face) {
        FT_UInt idx = FT_Get_Char_Index(s_ft_face, (FT_ULong)cp);
        if (idx && FT_Load_Glyph(s_ft_face, idx, FT_LOAD_RENDER | FT_LOAD_NO_HINTING) == 0) {
            FT_Bitmap &b = s_ft_face->glyph->bitmap;
            g.bm_w = (int)b.width; g.bm_h = (int)b.rows;
            g.bearing_x = s_ft_face->glyph->bitmap_left;
            g.bearing_y = s_ft_face->glyph->bitmap_top;
            g.bm.assign(b.buffer, b.buffer + b.width * b.rows);
        }
    }
    s_glyph_cache[cp] = std::move(g);
    return s_glyph_cache[cp];
}

// ============================================================================
// Keyboard queue
// ============================================================================
static std::queue<int> s_keys;
static std::mutex      s_keys_mx;

static void key_push(int c) { std::lock_guard<std::mutex> lk(s_keys_mx); s_keys.push(c); }
static int  key_pop()  {
    std::lock_guard<std::mutex> lk(s_keys_mx);
    if (s_keys.empty()) return -1;
    int c = s_keys.front(); s_keys.pop(); return c;
}

// ============================================================================
// SDL init / shutdown
// ============================================================================
bool gfx_sdl_init(const char *title, int w, int h) {
    s_win_w = w; s_win_h = h;
    s_init_win_w = w;  // Save initial dimensions
    s_init_win_h = h;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return false;
    }
    s_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!s_window) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return false; }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED);
    if (!s_renderer)
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // nearest-neighbour: crisp pixels

    ft_init();

    for (auto &row : s_grid)
        for (auto &c : row) c = {' ', 7, 0};

    s_needs_render = true;
    SDL_StartTextInput();
    return true;
}

void gfx_sdl_shutdown() {
    if (s_gfx_tex)  { SDL_DestroyTexture(s_gfx_tex); s_gfx_tex = nullptr; }
    if (s_ft_face)  { FT_Done_Face(s_ft_face);       s_ft_face = nullptr; }
    if (s_ft_lib)   { FT_Done_FreeType(s_ft_lib);    s_ft_lib  = nullptr; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = nullptr; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = nullptr; }
    SDL_Quit();
}

// ============================================================================
// Event pump  (must run on main thread)
// ============================================================================
static bool s_quit = false;   // set on SDL_QUIT, checked everywhere
static bool s_fullscreen = false;

// Toggle or set fullscreen state — defined in BASIC_NS to match the header declarations
BASIC_NS_BEGIN

void gfx_sdl_set_fullscreen(bool fs) {
    if (!s_window) return;
    s_fullscreen = fs;
    SDL_SetWindowFullscreen(s_window,
        fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    s_needs_render = true;
}

void gfx_sdl_toggle_fullscreen() {
    gfx_sdl_set_fullscreen(!s_fullscreen);
}

void gfx_palette_reset_pub() {
    gfx_palette_reset();
}

BASIC_NS_END

bool gfx_sdl_pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            s_quit = true;
            // Signal the interpreter to stop cleanly; the window stays
            // open until display_shutdown() is called after cleanup.
            BASIC_NS::g_break = 1;
            break;            // keep draining events, don't return false yet
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                s_win_w = e.window.data1;
                s_win_h = e.window.data2;
                ft_set_size_for_window();
                s_needs_render = true;
            }
            break;
        case SDL_KEYDOWN: {
            SDL_Keycode sym = e.key.keysym.sym;
            if (sym == SDLK_c && (e.key.keysym.mod & KMOD_CTRL)) {
                // Ctrl+C — copy selection to clipboard (or break if no selection)
                if (s_selecting && !s_selection_text.empty()) {
                    SDL_SetClipboardText(s_selection_text.c_str());
                    s_selecting = false;  // Clear selection after copy
                    s_needs_render = true;
                } else {
                    // No selection: send SIGINT break
                    BASIC_NS::g_break = 1;
                    key_push(3);
                }
            } else if (sym == SDLK_v && (e.key.keysym.mod & KMOD_CTRL)) {
                // Ctrl+V — paste from clipboard
                if (SDL_HasClipboardText()) {
                    char *clipboard = SDL_GetClipboardText();
                    if (clipboard) {
                        for (const char *p = clipboard; *p; p++)
                            key_push((unsigned char)*p);
                        SDL_free(clipboard);
                    }
                }
            } else if (sym == SDLK_F11) {
                // F11 — toggle fullscreen
                BASIC_NS::gfx_sdl_toggle_fullscreen();
            } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                key_push('\r');
            } else if (sym == SDLK_BACKSPACE) {
                key_push(8);
            } else if (sym == SDLK_TAB) {
                key_push(9);
            } else if (sym == SDLK_ESCAPE) {
                key_push(27);
            } else if (sym == SDLK_UP)    { key_push(0x1000); }
            else if (sym == SDLK_DOWN)    { key_push(0x1001); }
            else if (sym == SDLK_LEFT)    { key_push(0x1002); }
            else if (sym == SDLK_RIGHT)   { key_push(0x1003); }
            else if (sym == SDLK_PAGEUP && (e.key.keysym.mod & KMOD_SHIFT)) {
                // Shift+PageUp: scroll up in history
                int lines_in_scrollback = std::min(s_scrollback_count, SCROLLBACK_LINES);
                s_display_offset = std::min(s_display_offset + s_text_rows, lines_in_scrollback - s_text_rows);
                s_needs_render = true;
            }
            else if (sym == SDLK_PAGEDOWN && (e.key.keysym.mod & KMOD_SHIFT)) {
                // Shift+PageDown: scroll down in history (toward bottom)
                s_display_offset = std::max(0, s_display_offset - s_text_rows);
                s_needs_render = true;
            }
            break;
        }
        case SDL_TEXTINPUT:
            for (const char *p = e.text.text; *p; p++)
                key_push((unsigned char)*p);
            break;
        case SDL_MOUSEWHEEL:
            if (!s_gfx_active && (SDL_GetModState() & KMOD_CTRL)) {
                // Ctrl+scroll in SCREEN 0: resize font → changes cols/rows
                int delta = (e.wheel.y > 0) ? 1 : -1;
                if (s_font_size_override == 0)
                    s_font_size_override = s_cell_h;
                s_font_size_override = std::max(6, std::min(72, s_font_size_override + delta));
                ft_set_size_for_window();
                s_needs_render = true;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (!s_gfx_active && e.button.button == SDL_BUTTON_LEFT) {
                int col = (e.button.x - TEXT_PAD) / s_cell_w;
                int row = (e.button.y - TEXT_PAD) / s_cell_h;
                col = std::max(0, std::min(col, s_text_cols - 1));
                row = std::max(0, std::min(row, s_text_rows - 1));
                
                // If already have a selection, toggle it off
                if (s_selecting) {
                    s_selecting = false;
                    s_drag_occurred = false;
                    s_needs_render = true;
                } else {
                    // Start new selection
                    s_selecting = true;
                    s_drag_occurred = false;
                    s_sel_start_row = s_sel_end_row = row;
                    s_sel_start_col = s_sel_end_col = col;
                    update_selection_text();
                    s_needs_render = true;
                }
            }
            break;
        case SDL_MOUSEMOTION:
            if (!s_gfx_active && s_selecting) {
                // Update selection end point
                int col = (e.motion.x - TEXT_PAD) / s_cell_w;
                int row = (e.motion.y - TEXT_PAD) / s_cell_h;
                col = std::max(0, std::min(col, s_text_cols - 1));
                row = std::max(0, std::min(row, s_text_rows - 1));
                
                // Check if we actually moved
                if (row != s_sel_start_row || col != s_sel_start_col) {
                    s_drag_occurred = true;
                    s_sel_end_row = row;
                    s_sel_end_col = col;
                    update_selection_text();
                    s_needs_render = true;
                }
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (!s_gfx_active && e.button.button == SDL_BUTTON_LEFT) {
                // If no drag occurred, clear selection
                if (!s_drag_occurred) {
                    s_selecting = false;
                    s_needs_render = true;
                }
                // If drag occurred, keep selection visible
                s_drag_occurred = false;
            }
            break;
        default: break;
        }
    }
    return !s_quit;
}

// ============================================================================
// Rendering
// ============================================================================
static void render_text_cell(int row, int col) {
    const Cell cell = scrollback_get_cell(row, col);
    unsigned char ch = (unsigned char)cell.ch;

    int cx = col * s_cell_w;
    int cy = row * s_cell_h;

    // Determine if this cell is selected
    bool selected = is_cell_selected(row, col);
    Uint32 bg = s_pal[cell.bg & 15];

    // In text mode, always draw the cell background (even spaces) so colored
    // backgrounds show and old glyphs are properly erased.
    // In graphics mode, only draw bg for cells with actual content so the
    // graphics pixel buffer shows through empty cells.
    if (!s_gfx_active || (ch != 0 && ch != ' ' && ch != 255)) {
        SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(s_renderer,
            (bg>>16)&0xFF, (bg>>8)&0xFF, bg&0xFF, 255);
        SDL_Rect bgr = { cx, cy, s_cell_w, s_cell_h };
        SDL_RenderFillRect(s_renderer, &bgr);
    }

    // The block characters fill exact parts of the cell, so neighbouring
    // cells join up (Nibbles draws its whole playfield with these); the
    // font's own versions leave gaps between rows.
    if (ch >= 219 && ch <= 223) {
        Uint32 fg = s_pal[cell.fg & 15];
        SDL_Rect r = { cx, cy, s_cell_w, s_cell_h };
        if (ch == 220) { r.y += s_cell_h / 2; r.h -= s_cell_h / 2; }      // lower half
        else if (ch == 221) r.w = s_cell_w / 2;                           // left half
        else if (ch == 222) { r.x += s_cell_w / 2; r.w -= s_cell_w / 2; } // right half
        else if (ch == 223) r.h = s_cell_h / 2;                           // upper half
        SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(s_renderer, (fg>>16)&0xFF, (fg>>8)&0xFF, fg&0xFF, 255);
        SDL_RenderFillRect(s_renderer, &r);
    }
    // Draw the character if it's printable
    else if (ch != 0 && ch != ' ' && ch != 255) {
        const Glyph &g = glyph_get(s_cp437[ch]);
        if (!g.bm.empty()) {
            // Cache keyed by (codepoint, font generation, palette generation, fg color)
            struct Key {
                int cp;
                int gen;
                int pal_gen;
                int fg;
                bool operator==(const Key &o) const {
                    return cp == o.cp && gen == o.gen && pal_gen == o.pal_gen && fg == o.fg;
                }
            };

            struct KeyHash {
                size_t operator()(const Key &k) const {
                    return (size_t)k.cp ^ ((size_t)k.gen << 16) ^ ((size_t)k.pal_gen << 20) ^ ((size_t)k.fg << 24);
                }
            };

            static std::unordered_map<Key, SDL_Texture*, KeyHash> tex_cache;

            Key key{ ch, s_font_gen, s_pal_gen, cell.fg & 15 };
            SDL_Texture *tex = nullptr;

            auto it = tex_cache.find(key);
            if (it != tex_cache.end()) {
                tex = it->second;
            } else {
                tex = SDL_CreateTexture(
                    s_renderer,
                    SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STATIC,
                    g.bm_w,
                    g.bm_h
                );
                if (tex) {
                    std::vector<Uint32> argb((size_t)(g.bm_w * g.bm_h));

                    Uint32 fgcol = s_pal[cell.fg & 15]; // ARGB from palette

                    for (int i = 0; i < g.bm_w * g.bm_h; i++) {
                        Uint8 a = g.bm[i];
                        // Use full alpha for text rendering — antialiased gray values
                        // look invisible against dark backgrounds at small font sizes.
                        // Threshold at 64: anything above renders fully opaque.
                        Uint8 out_a = (a >= 64) ? 255 : 0;
                        argb[i] = (fgcol & 0x00FFFFFFu) | ((Uint32)out_a << 24);
                    }

                    SDL_UpdateTexture(tex, nullptr, argb.data(), g.bm_w * 4);
                    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

                    tex_cache[key] = tex;
                }
            }

            if (tex) {
                // Baseline alignment
                int ascender = s_ft_face
                    ? (int)(s_ft_face->size->metrics.ascender >> 6)
                    : s_cell_h - 2;

                int gx = cx + g.bearing_x;
                int gy = cy + ascender - g.bearing_y;

                SDL_Rect dst = { gx, gy, g.bm_w, g.bm_h };
                SDL_RenderCopy(s_renderer, tex, nullptr, &dst);
            }
        }
    }

    // Draw semi-transparent selection highlight on top
    if (selected) {
        SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(s_renderer, 100, 150, 255, 100);  // Light blue, semi-transparent
        SDL_Rect sel = { cx, cy, s_cell_w, s_cell_h };
        SDL_RenderFillRect(s_renderer, &sel);
    }
}



void gfx_sdl_render() {
    if (!s_needs_render) return;
    s_needs_render = false;

    // Clear full window
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);

    //
    // 1. Draw graphics framebuffer (if active)
    //
    if (s_gfx_active && s_gfx_tex) {
        SDL_RenderSetViewport(s_renderer, nullptr);
        // Render from the visible page (vpage), not necessarily the draw page
        const std::vector<Uint32> &vpx = (s_vpage < GFX_MAX_PAGES && !s_pages[s_vpage].empty())
                                          ? s_pages[s_vpage] : s_pages[0];
        
        // Use static buffer to avoid allocation per frame
        static std::vector<Uint32> tex_pixels;
        if (tex_pixels.size() != vpx.size())
            tex_pixels.resize(vpx.size());
        
        // Set alpha channel for all pixels in one pass
        for (size_t i = 0; i < vpx.size(); i++)
            tex_pixels[i] = vpx[i] | 0xFF000000u;
        
        SDL_UpdateTexture(s_gfx_tex, nullptr, tex_pixels.data(), s_gfx_w * 4);
        // Stretch to window — window height already accounts for pixel aspect ratio
        SDL_Rect dst = { 0, 0, s_win_w, s_win_h };
        SDL_RenderCopy(s_renderer, s_gfx_tex, nullptr, &dst);
    }

    //
    // 2. Draw text grid overlay (ALWAYS, even in graphics modes)
    //
    // QBASIC draws text at the top-left, never centered.
    //
    SDL_Rect vp = { 0, 0, s_text_cols * s_cell_w, s_text_rows * s_cell_h };
    SDL_RenderSetViewport(s_renderer, &vp);
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);

    for (int r = 0; r < s_text_rows; r++)
        for (int c = 0; c < s_text_cols; c++)
            render_text_cell(r, c);

    //
    // 3. Draw cursor (in ALL modes)
    //
    if (s_cursor_vis &&
        s_cur_row < s_text_rows &&
        s_cur_col < s_text_cols)
    {
        int cx = s_cur_col * s_cell_w;
        int cy = s_cur_row * s_cell_h + s_cell_h - 3;

        SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(s_renderer, 200, 200, 200, 255);

        SDL_RenderDrawLine(s_renderer, cx, cy, cx + s_cell_w - 1, cy);
        SDL_RenderDrawLine(s_renderer, cx, cy + 1, cx + s_cell_w - 1, cy + 1);
    }

    // Reset viewport back to full window
    SDL_RenderSetViewport(s_renderer, nullptr);

    //
    // 4. Present final composed frame
    //
    SDL_RenderPresent(s_renderer);
}


void gfx_sdl_mark_dirty() { s_needs_render = true; }

// ============================================================================
// Pixel helpers
// Palette mode:   0xIIRRGGBB where II = palette index (0-15) in alpha slot.
// Truecolor mode: 0xFFRRGGBB — full ARGB, alpha always 0xFF.
// ============================================================================
static inline Uint32 color_to_pixel(int color) {
    if (s_truecolor) {
        // color is 0x00RRGGBB from _RGB() or similar
        return 0xFF000000u | (Uint32)(color & 0x00FFFFFFu);
    } else {
        int idx = color & 15;
        return ((Uint32)idx << 24) | (s_pal[idx] & 0x00FFFFFFu);
    }
}

static inline void px_set(int x, int y, int color) {
    if (x < 0 || y < 0 || x >= s_gfx_w || y >= s_gfx_h) return;
    s_pages[s_apage][(size_t)(y * s_gfx_w + x)] = color_to_pixel(color);
}

static inline Uint32 px_get(int x, int y) {
    if (x < 0 || y < 0 || x >= s_gfx_w || y >= s_gfx_h) return 0;
    return s_pages[s_apage][(size_t)(y * s_gfx_w + x)];
}

// Extract the stored palette index from a pixel value (palette mode only)
static inline int px_index(Uint32 pixel) {
    return (int)((pixel >> 24) & 0xFF) & 15;
}

// ============================================================================
// Text grid helpers
// ============================================================================
static void text_scroll_up() {
    scrollback_push_line();
    s_display_offset = 0;  // Auto-scroll to bottom when new content appears
    s_needs_render = true;
}

static void text_newline() {
    s_cur_col = 0;
    if (++s_cur_row >= s_text_rows) { text_scroll_up(); s_cur_row = s_text_rows - 1; }
}

// ESC-sequence filter state — swallows ESC + any following non-alpha bytes
// then one alpha terminator.  Handles ESC E (CP/M clear), ESC [ ... (ANSI).
static int s_esc_state = 0;  // 0=normal, 1=saw ESC, 2=inside CSI

static void text_putchar(char c) {
    unsigned char uc = (unsigned char)c;

    if (s_esc_state == 1) {
        if (uc == '[') { s_esc_state = 2; return; }  // CSI sequence
        s_esc_state = 0; return;                      // single-char ESC (ESC E etc.) — discard
    }
    if (s_esc_state == 2) {
        // Inside CSI: consume until alpha terminator
        if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z')) s_esc_state = 0;
        return;
    }
    if (uc == 0x1B) { s_esc_state = 1; return; }

    if (c == '\n') { text_newline(); return; }
    if (c == '\r') { s_cur_col = 0;  return; }
    // Control codes QBasic shows as CP437 symbols (smileys, arrows, ...);
    // the rest (bell, tab, cursor moves, ESC) aren't put on the screen.
    if (uc < 0x20 && !((uc >= 1 && uc <= 6) || (uc >= 14 && uc <= 26))) return;
    // A character in the last column leaves the cursor just past it; the
    // wrap (and a scroll, on the bottom row) waits for the next character,
    // as in QBasic -- so PRINT CHR$(219); at row 25, column 80 doesn't
    // scroll the screen.
    if (s_cur_col >= s_text_cols) text_newline();
    if (s_cur_col < s_text_cols && s_cur_row < s_text_rows)
        s_grid[s_cur_row][s_cur_col] = {c, (Uint8)s_cur_fg, (Uint8)s_cur_bg};
    s_cur_col++;
    s_needs_render = true;
}

// ============================================================================
// basic_gfx.h implementation
// ============================================================================

// Screen mode dims table — pixel buffer dimensions per QBasic spec
static void screen_mode_dims(int mode, int *w, int *h) {
    static const struct { int m, w, h; } MODES[] = {
        { 1, 320, 200}, { 2, 640, 200}, { 3, 720, 348}, { 4, 640, 400},
        { 5, 160, 100}, { 6, 160, 200}, { 7, 320, 200}, { 8, 640, 200},
        { 9, 640, 350}, {10, 640, 350}, {11, 640, 480}, {12, 640, 480},
        {13, 320, 200}, {14, 320, 200}, {15, 640, 200}, {16, 640, 480},
        {17, 640, 480}, {18, 640, 480}, {19, 640, 480}, {20, 512, 480},
        {21, 640, 400}, {22, 640, 480}, {23, 800, 600}, {24, 160, 200},
        {25, 320, 200}, {26, 640, 200}, {27, 640, 200}, {28, 720, 350},
        {0, 0, 0}
    };
    *w = 640; *h = 350;
    for (int i = 0; MODES[i].m || MODES[i].w; i++)
        if (MODES[i].m == mode) { *w = MODES[i].w; *h = MODES[i].h; return; }
}

void gfx_screen_tc(int w, int h) {
    // Truecolor SCREEN — no palette, direct 32-bit ARGB pixels
    s_truecolor = true;
    gfx_palette_reset();
    s_gfx_w = w; s_gfx_h = h;
    // Size window to fit display
    {
        SDL_DisplayMode dm;
        int disp = s_window ? SDL_GetWindowDisplayIndex(s_window) : 0;
        if (SDL_GetCurrentDisplayMode(disp, &dm) != 0) { dm.w = 1920; dm.h = 1080; }
        int max_w = (int)(dm.w * 0.92f), max_h = (int)(dm.h * 0.92f);
        int scale = 1;
        while ((w*(scale+1)) <= max_w && (h*(scale+1)) <= max_h) scale++;
        s_win_w = w * scale; s_win_h = h * scale;
        if (s_window) SDL_SetWindowSize(s_window, s_win_w, s_win_h);
    }
    for (int i = 0; i < GFX_MAX_PAGES; i++) s_pages[i].assign((size_t)(w * h), 0xFF000000u);
    if (s_gfx_tex) SDL_DestroyTexture(s_gfx_tex);
    s_gfx_tex = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureBlendMode(s_gfx_tex, SDL_BLENDMODE_NONE);
    s_gfx_active = true;
    s_text_cols = TEXT_COLS_DEF; s_text_rows = TEXT_ROWS_DEF;
    ft_set_size_for_window();
    s_needs_render = true;
}

void gfx_screen(int mode) {
    gfx_screen_ex(mode, 0, -1, -1);
}

void gfx_screen_ex(int mode, int /*colorswitch*/, int apage, int vpage) {
    s_truecolor = false;
    gfx_palette_reset();   // SCREEN always resets palette to CGA defaults
    if (mode == 0) {
        s_gfx_active = false;
        if (s_gfx_tex) { SDL_DestroyTexture(s_gfx_tex); s_gfx_tex = nullptr; }
        for (int i = 0; i < GFX_MAX_PAGES; i++) s_pages[i].clear();
        s_gfx_w = s_gfx_h = 0;
        s_apage = s_vpage = 0;
        // Restore window to initial size
        if (s_window) SDL_SetWindowSize(s_window, s_init_win_w, s_init_win_h);
        s_win_w = s_init_win_w;
        s_win_h = s_init_win_h;
        ft_set_size_for_window();
        s_needs_render = true;
        return;
    }
    int gw, gh;
    screen_mode_dims(mode, &gw, &gh);

    // Only reallocate pages if the mode dimensions changed
    bool dims_changed = (s_gfx_w != gw || s_gfx_h != gh);
    s_gfx_w = gw; s_gfx_h = gh;

    // All CGA/EGA modes should display with 4:3 aspect ratio (like real monitors)
    // Calculate display height to maintain 4:3 ratio
    int display_h = (gw * 3) / 4;   // 4:3 aspect ratio

    if (dims_changed) {
        // Resize window to best integer scale that fits the display
        SDL_DisplayMode dm;
        int disp = s_window ? SDL_GetWindowDisplayIndex(s_window) : 0;
        if (SDL_GetCurrentDisplayMode(disp, &dm) != 0) { dm.w = 1920; dm.h = 1080; }
        int max_w = (int)(dm.w * 0.92f);
        int max_h = (int)(dm.h * 0.92f);
        int scale = 1;
        while ((gw * (scale + 1)) <= max_w && (display_h * (scale + 1)) <= max_h)
            scale++;
        s_win_w = gw * scale;
        s_win_h = display_h * scale;
        if (s_window) SDL_SetWindowSize(s_window, s_win_w, s_win_h);

        // Allocate all pages
        for (int i = 0; i < GFX_MAX_PAGES; i++)
            s_pages[i].assign((size_t)(gw * gh), color_to_pixel(0));

        if (s_gfx_tex) SDL_DestroyTexture(s_gfx_tex);
        s_gfx_tex = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, gw, gh);
        SDL_SetTextureBlendMode(s_gfx_tex, SDL_BLENDMODE_BLEND);
    }

    // Update active/visible pages (only if explicitly specified)
    if (apage >= 0 && apage < GFX_MAX_PAGES) s_apage = apage;
    if (vpage >= 0 && vpage < GFX_MAX_PAGES) s_vpage = vpage;

    s_gfx_active = true;
    s_text_cols = TEXT_COLS_DEF;
    s_text_rows = TEXT_ROWS_DEF;
    ft_set_size_for_window();
    s_needs_render = true;
}

void gfx_palette(int idx, int r, int g, int b) {
    idx &= 15;
    s_pal[idx] = 0xFF000000u
               | ((Uint32)(r & 0xFF) << 16)
               | ((Uint32)(g & 0xFF) <<  8)
               | ((Uint32)(b & 0xFF)      );
    s_pal_gen++;
    // Update RGB of any pixel in the buffer already using this index
    Uint32 new_rgb = s_pal[idx] & 0x00FFFFFFu;
    Uint32 tag     = (Uint32)idx << 24;
    for (int _pi = 0; _pi < GFX_MAX_PAGES; _pi++) for (auto &px : s_pages[_pi]) {
        if ((px >> 24) == (Uint32)idx)
            px = tag | new_rgb;
    }
    s_needs_render = true;
}

void gfx_cls(int color) {
    // In SCREEN 0, CLS clears to the COLOR statement's background.
    if (!s_gfx_active) color = s_cur_bg;
    // Update the current background colour so subsequent text output
    // (PRINT, LOCATE, etc.) uses the same colour as the cleared screen.
    s_cur_bg = color & 15;

    // 1. Clear graphics buffer if active
    if (s_gfx_active) {
        int ci = color & 15;
        Uint32 fill = ((Uint32)ci << 24) | (s_pal[ci] & 0x00FFFFFFu);
        std::fill(s_pages[s_apage].begin(), s_pages[s_apage].end(), fill);
    }

    // 2. Clear text grid to the same background colour
    for (int r = 0; r < s_text_rows; r++) {
        for (int c = 0; c < s_text_cols; c++) {
            s_grid[r][c] = { ' ', (Uint8)s_cur_fg, (Uint8)(color & 15) };
        }
    }

    // 3. Reset cursor position (but leave visibility state alone — CLS doesn't change input mode)
    s_cur_row    = 0;
    s_cur_col    = 0;

    // 4. Mark dirty so it actually redraws
    s_needs_render = true;
}

// CLS 2: clear the text viewport (rows top..bottom, from 1), and in a
// graphics mode the pixels under it, as QBasic does.
void gfx_cls_text(int top, int bottom) {
    if (top < 1) top = 1;
    if (bottom > s_text_rows) bottom = s_text_rows;
    if (top > bottom) return;
    for (int r = top - 1; r < bottom; r++)
        for (int c = 0; c < s_text_cols; c++)
            s_grid[r][c] = { ' ', (Uint8)s_cur_fg, (Uint8)s_cur_bg };
    if (s_gfx_active && s_text_rows > 0 && !s_pages[s_apage].empty()) {
        int y0 = (top - 1) * s_gfx_h / s_text_rows;
        int y1 = bottom * s_gfx_h / s_text_rows;
        Uint32 fill = ((Uint32)0 << 24) | (s_pal[0] & 0x00FFFFFFu);
        std::fill(s_pages[s_apage].begin() + (size_t)y0 * s_gfx_w,
                  s_pages[s_apage].begin() + (size_t)y1 * s_gfx_w, fill);
    }
    s_cur_row = top - 1;
    s_cur_col = 0;
    s_needs_render = true;
}

void gfx_pset(int x, int y, int color) {
    px_set(x, y, color);
    s_needs_render = true;
}

int gfx_point(int x, int y) {
    if (!s_gfx_active || x < 0 || y < 0 || x >= s_gfx_w || y >= s_gfx_h) return -1;
    return px_index(s_pages[s_apage][(size_t)(y * s_gfx_w + x)]);
}

void gfx_line(int x0, int y0, int x1, int y1, int color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px_set(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    s_needs_render = true;
}

void gfx_box(int x1, int y1, int x2, int y2, int color) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    gfx_line(x1, y1, x2, y1, color);
    gfx_line(x2, y1, x2, y2, color);
    gfx_line(x2, y2, x1, y2, color);
    gfx_line(x1, y2, x1, y1, color);
    s_needs_render = true;
}

void gfx_boxfill(int x1, int y1, int x2, int y2, int color) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(x1, 0); y1 = std::max(y1, 0);
    x2 = std::min(x2, s_gfx_w - 1); y2 = std::min(y2, s_gfx_h - 1);
    int ci = color & 15;
    Uint32 col = ((Uint32)ci << 24) | (s_pal[ci] & 0x00FFFFFFu);
    for (int y = y1; y <= y2; y++) {
        Uint32 *row = s_pages[s_apage].data() + (size_t)(y * s_gfx_w);
        std::fill(row + x1, row + x2 + 1, col);
    }
    s_needs_render = true;
}

// An ellipse (QBasic's CIRCLE with an aspect ratio), midpoint algorithm.
void gfx_ellipse(int cx, int cy, int rx, int ry, int color) {
    if (rx < 0 || ry < 0) return;
    if (rx == 0 || ry == 0) {                    // degenerate: a line
        for (int i = -rx; i <= rx; i++) px_set(cx + i, cy, color);
        for (int i = -ry; i <= ry; i++) px_set(cx, cy + i, color);
        s_needs_render = true;
        return;
    }
    long long a2 = (long long)rx * rx, b2 = (long long)ry * ry;
    long long x = 0, y = ry;
    long long d = b2 - a2 * ry + a2 / 4;
    auto plot4 = [&](long long px, long long py) {
        px_set(cx + (int)px, cy + (int)py, color); px_set(cx - (int)px, cy + (int)py, color);
        px_set(cx + (int)px, cy - (int)py, color); px_set(cx - (int)px, cy - (int)py, color);
    };
    while (b2 * x <= a2 * y) {                   // region 1: |slope| < 1
        plot4(x, y);
        if (d < 0) d += b2 * (2 * x + 3);
        else { d += b2 * (2 * x + 3) + a2 * (2 - 2 * y); y--; }
        x++;
    }
    d = b2 * (x * x + x) + a2 * (y - 1) * (y - 1) - a2 * b2;
    while (y >= 0) {                             // region 2
        plot4(x, y);
        if (d > 0) d += a2 * (3 - 2 * y);
        else { d += b2 * (2 * x + 2) + a2 * (3 - 2 * y); x++; }
        y--;
    }
    s_needs_render = true;
}

void gfx_circle(int cx, int cy, int radius, int color) {
    gfx_ellipse(cx, cy, radius, radius, color);
}

// An arc of an ellipse, QBasic style: angles in radians, 0 = right (3
// o'clock), counter-clockwise; a negative angle also draws the radius to
// that end (a pie slice).
static inline int iround(double v) { return (int)(v < 0 ? v - 0.5 : v + 0.5); }

void gfx_ellipse_arc(int cx, int cy, int rx, int ry, double start_a, double end_a, int color) {
    if (rx <= 0 && ry <= 0) return;
    const double PI2 = 6.283185307179586;
    bool line_s = start_a < 0, line_e = end_a < 0;
    double sa = fabs(start_a), ea = fabs(end_a);
    while (sa >= PI2) sa -= PI2;
    while (ea >= PI2) ea -= PI2;
    if (ea <= sa) ea += PI2;                     // the arc wraps around 0
    int r = std::max(rx, ry);
    int steps = (int)(r * (ea - sa) * 1.5) + 8;
    double step = (ea - sa) / steps;
    int lx = 0, ly = 0;
    for (int i = 0; i <= steps; i++) {
        double angle = sa + i * step;
        int px = cx + iround(rx * cos(angle));
        int py = cy - iround(ry * sin(angle));
        if (i > 0) gfx_line(lx, ly, px, py, color); else px_set(px, py, color);
        lx = px; ly = py;
    }
    if (line_s) gfx_line(cx, cy, cx + iround(rx * cos(sa)), cy - iround(ry * sin(sa)), color);
    if (line_e) gfx_line(cx, cy, cx + iround(rx * cos(ea)), cy - iround(ry * sin(ea)), color);
    s_needs_render = true;
}

void gfx_arc(int cx, int cy, int radius, double start_a, double end_a, int color) {
    gfx_ellipse_arc(cx, cy, radius, radius, start_a, end_a, color);
}

void gfx_paint(int x, int y, int fill_color, int border_color) {
    if (x < 0 || y < 0 || x >= s_gfx_w || y >= s_gfx_h) return;
    Uint32 fill   = color_to_pixel(fill_color);
    Uint32 border = color_to_pixel(border_color);

    /* Border fill: paint all connected pixels that are NOT the border color.
     * Unlike flood fill, interior pixel colors are irrelevant — only the
     * border color stops the fill. Seed must not be border or already filled. */
    if (px_get(x, y) == border || px_get(x, y) == fill) return;

    struct Span { int x0, x1, y, dy; };
    std::vector<Span> stk;
    stk.push_back({x, x, y,      1});
    stk.push_back({x, x, y - 1, -1});

    while (!stk.empty()) {
        auto [x0, x1, sy, dy] = stk.back(); stk.pop_back();
        if (sy < 0 || sy >= s_gfx_h) continue;

        /* Extend left until border */
        int nx0 = x0;
        while (nx0 > 0 && px_get(nx0 - 1, sy) != border) nx0--;
        /* Extend right until border */
        int nx1 = x1;
        while (nx1 < s_gfx_w - 1 && px_get(nx1 + 1, sy) != border) nx1++;

        /* Fill the span */
        for (int i = nx0; i <= nx1; i++)
            s_pages[s_apage][(size_t)(sy * s_gfx_w + i)] = fill;

        /* Push child spans in forward direction */
        int ny = sy + dy;
        if (ny >= 0 && ny < s_gfx_h) {
            int i = nx0;
            while (i <= nx1) {
                while (i <= nx1 && px_get(i, ny) == border) i++;
                if (i > nx1) break;
                int js = i;
                while (i <= nx1 && px_get(i, ny) != border) i++;
                stk.push_back({js, i - 1, ny, dy});
            }
        }

        /* Push spans that extend beyond seed range in reverse direction */
        ny = sy - dy;
        if (ny >= 0 && ny < s_gfx_h) {
            int i = nx0;
            while (i <= nx1) {
                while (i <= nx1 && px_get(i, ny) == border) i++;
                if (i > nx1) break;
                int js = i;
                while (i <= nx1 && px_get(i, ny) != border) i++;
                if (js < x0 || i - 1 > x1)
                    stk.push_back({js, i - 1, ny, -dy});
            }
        }
    }
    s_needs_render = true;
}

void gfx_get(int id, int x1, int y1, int x2, int y2) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    Sprite &sp = s_sprites[id];
    sp.w = w; sp.h = h;
    sp.px.resize((size_t)(w * h));
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            sp.px[(size_t)(row * w + col)] = px_get(x1 + col, y1 + row);
    sp.bg_color = s_cur_bg & 15;
}

int gfx_sprite_exists(int id) { return s_sprites.count(id) ? 1 : 0; }

// One pixel of a PUT: the sprite's colour index ci combined with the screen.
static void put_pixel(int dx, int dy, int ci, int mode) {
    if (dx < 0 || dy < 0 || dx >= s_gfx_w || dy >= s_gfx_h) return;
    size_t pidx = (size_t)(dy * s_gfx_w + dx);
    int old = px_index(s_pages[s_apage][pidx]) & 15;
    int c;
    switch (mode) {
        case GFX_PUT_XOR:    c = old ^ ci; break;
        case GFX_PUT_PRESET: c = ~ci;      break;
        case GFX_PUT_AND:    c = old & ci; break;
        case GFX_PUT_OR:     c = old | ci; break;
        default:             c = ci;       break;   // PSET: the whole rectangle, background too
    }
    c &= 15;
    s_pages[s_apage][pidx] = ((Uint32)c << 24) | (s_pal[c] & 0x00FFFFFFu);
}

void gfx_put(int id, int x, int y, int mode) {
    auto it = s_sprites.find(id);
    if (it == s_sprites.end()) return;
    const Sprite &sp = it->second;
    for (int row = 0; row < sp.h; row++)
        for (int col = 0; col < sp.w; col++)
            put_pixel(x + col, y + row, px_index(sp.px[(size_t)(row * sp.w + col)]), mode);
    s_needs_render = true;
}

void gfx_put_array(const int *raw_longs, int count, int x, int y, int mode) {
    if (!s_gfx_active || !raw_longs || count < 1) return;

    // GW-BASIC / QB GET array format for EGA SCREEN 9 (and similar planar modes):
    //   Bytes 0-1 (word LE): pixel width
    //   Bytes 2-3 (word LE): pixel height
    //   Remaining bytes: 4 EGA bit-planes stored row by row.
    //     For each row: plane0_bytes, plane1_bytes, plane2_bytes, plane3_bytes
    //     where bytes_per_plane_row = ceil(width / 8), 1 bit per pixel, MSB = leftmost pixel.
    //   Color index for pixel col = bit col from each plane's byte(s), assembled as:
    //     color = plane0_bit | (plane1_bit<<1) | (plane2_bit<<2) | (plane3_bit<<3)

    const unsigned char *raw = reinterpret_cast<const unsigned char *>(raw_longs);
    int total_bytes = count * 4;

    int w = (int)(raw[0] | (raw[1] << 8));
    int h = (int)(raw[2] | (raw[3] << 8));
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return;

    int bpp_row = (w + 7) / 8;          // bytes per row per plane
    int row_stride = bpp_row * 4;       // 4 planes per row
    int needed = 4 + row_stride * h;
    if (needed > total_bytes) return;

    const unsigned char *pixels = raw + 4;

    for (int row = 0; row < h; row++) {
        const unsigned char *row_base = pixels + row * row_stride;
        for (int col = 0; col < w; col++) {
            int byte_off = col / 8;
            int bit      = 7 - (col % 8);   // MSB = leftmost pixel
            int pal_idx  = 0;
            for (int p = 0; p < 4; p++) {
                const unsigned char *plane = row_base + p * bpp_row;
                if (byte_off < bpp_row)
                    pal_idx |= (((plane[byte_off] >> bit) & 1) << p);
            }
            put_pixel(x + col, y + row, pal_idx & 15, mode);
        }
    }
    s_needs_render = true;
}

int gfx_active(void) { return s_gfx_active ? 1 : 0; }
int gfx_width(void)  { return s_gfx_w; }
int gfx_height(void) { return s_gfx_h; }

// ============================================================================
// display.h implementation  (replaces display_ansi.cpp)
// ============================================================================
BASIC_NS_BEGIN

void display_init(void) {
    gfx_sdl_init("Felix BASIC", 800, 600);
    scrollback_init();
}

void display_shutdown(void) {
    gfx_sdl_shutdown();
}

void display_cls(void) {
/*#ifdef WOPR
    if (g_wopr.visible && g_wopr.phase == WoprPhase::PLAYING_BASIC) {
        wopr_basic_cls();
        return;
    }
#endif*/
    scrollback_init();
    gfx_cls(s_cur_bg);
}

void display_locate(int row, int col) {
/*#ifdef WOPR
    if (g_wopr.visible && g_wopr.phase == WoprPhase::PLAYING_BASIC) {
        wopr_basic_locate(row, col);
        return;
    }
#endif */
    if (row > 0) s_cur_row = std::min(row - 1, s_text_rows - 1);
    if (col > 0) s_cur_col = std::min(col - 1, s_text_cols - 1);
}

void display_color(int fg, int bg) {
/*#ifdef WOPR
    if (g_wopr.visible && g_wopr.phase == WoprPhase::PLAYING_BASIC) {
        wopr_basic_color(fg, bg);
        return;
    }
#endif */
    s_cur_fg = fg & 15;
    s_cur_bg = bg & 15;
}

void display_width(int cols) {
    if (s_text_lock_rows <= 0) s_text_lock_rows = TEXT_ROWS_DEF;
    s_text_cols = (cols <= 40) ? 40 : 80;
    s_needs_render = true;
    if (!s_gfx_active) ft_set_size_for_window();
}

// WIDTH , rows: 25, 43 or 50 text rows (applied by the display_width call
// that follows).
void display_text_rows(int rows) {
    if (rows == 25 || rows == 43 || rows == 50) s_text_lock_rows = rows;
}

void display_print(char *s) {
#ifndef WOPR
    for (; *s; s++) text_putchar(*s);
    // Note: Do NOT pump/render here. This gets called on every PSET.
    // Batching is handled at the BASIC interpreter level (main loop).
#endif
}

void display_putchar(int c) {
    text_putchar((char)c);
    // Rendering batched at interpreter level, not here
}

void display_newline(void) {
    text_newline();
    // Rendering batched at interpreter level, not here
}

void display_spc(int n) {
    for (int i = 0; i < n; i++) text_putchar(' ');
}

void display_cursor(int visible) {
    // In graphics mode the cursor is managed solely by display_getline;
    // PRINT/INPUT calls to display_cursor must not interfere.
    if (s_gfx_active) return;
    s_cursor_vis = (visible != 0);
    s_needs_render = true;
}

int display_get_width(void) {
    return s_text_cols;
}

// Non-blocking key poll (INKEY$)
int display_inkey(void) {
    static Uint32 s_last_inkey_render = 0;
    Uint32 now = SDL_GetTicks();
    
    // Only render every ~16ms (60 FPS) during INKEY polling
    if (now - s_last_inkey_render >= 16) {
        gfx_sdl_pump();
        if (!BASIC_NS::basic_paced()) gfx_sdl_render();
        s_last_inkey_render = now;
    }
    
    int c = key_pop();
    if (c < 0) {
        SDL_Delay(5);   // yield to OS
        return 0;
    }
    return c;
}

// Blocking single char
int display_getchar(void) {
    int c = -1;
    while (c < 0) {
        if (!gfx_sdl_pump()) return 0;
        gfx_sdl_render();
        c = key_pop();
        if (c < 0) SDL_Delay(10);
    }
    return c;
}

// Blocking line input with echo and command history
int display_getline(char *buf, int bufsz) {
    #define SDL_HIST_MAX 64
    static char s_hist[SDL_HIST_MAX][512];
    static int  s_hist_count = 0;
    static int  s_hist_head  = 0;

    // Working buffer separate from buf so we can cancel edits
    char tmp[512] = "";
    char saved[512] = "";
    int  len    = 0;
    int  cursor = 0;
    int  hist_pos = s_hist_count;

    if (s_cur_col >= s_text_cols) text_newline();   // a wrap left pending
    // Remember where on the grid this input line started
    int start_row = s_cur_row;
    int start_col = s_cur_col;

    s_cursor_vis = true;
    s_needs_render = true;

    // Helper: redraw the input line in the grid from start_col
    auto redraw = [&]() {
        // Rewrite from start position
        int col = start_col;
        int row = start_row;
        for (int i = 0; i < len; i++) {
            if (col < s_text_cols && row < s_text_rows)
                s_grid[row][col] = {tmp[i], (Uint8)s_cur_fg, (Uint8)s_cur_bg};
            if (++col >= s_text_cols) { col = 0; row++; }
        }
        // Erase any old chars after new end
        int ecol = col, erow = row;
        for (int i = 0; i < 80; i++) {   // clear up to 80 chars of tail
            if (ecol < s_text_cols && erow < s_text_rows)
                s_grid[erow][ecol] = {' ', (Uint8)s_cur_fg, (Uint8)s_cur_bg};
            if (++ecol >= s_text_cols) { ecol = 0; erow++; }
        }
        // Place display cursor at edit cursor position
        int ccol = start_col + cursor;
        s_cur_row = start_row + ccol / s_text_cols;
        s_cur_col = ccol % s_text_cols;
        s_needs_render = true;
    };

    for (;;) {
        // Exit immediately if the window was closed
        if (s_quit) { buf[0] = '\0'; return -1; }

        // Blink cursor
        static Uint32 blink_t = 0;
        Uint32 now = SDL_GetTicks();
        if (now - blink_t > 500) {
            s_cursor_vis = !s_cursor_vis;
            blink_t = now;
            s_needs_render = true;
        }
        gfx_sdl_render();

        if (!gfx_sdl_pump()) { buf[0] = '\0'; return -1; }

        int c = key_pop();
        if (c < 0) { SDL_Delay(10); continue; }

#ifndef BASIC_BAREMETAL
        if (c == 3) {
            // Ctrl+C at the prompt closes BASIC, as in a terminal
            BASIC_NS::g_break = 0;
            text_newline();
            buf[0] = '\0';
            return -1;
        }
#endif
        if (c == '\n' || c == '\r') {
            // Commit: advance cursor past the typed text
            s_cur_col = start_col + len;
            s_cur_row = start_row + s_cur_col / s_text_cols;
            s_cur_col = s_cur_col % s_text_cols;
            text_newline();
            break;

        } else if (c == 8 || c == 127) {   // Backspace
            if (cursor > 0) {
                memmove(tmp + cursor - 1, tmp + cursor, len - cursor);
                cursor--; len--;
                redraw();
            }

        } else if (c == 0x1002) {   // Left arrow
            if (cursor > 0) { cursor--; redraw(); }

        } else if (c == 0x1003) {   // Right arrow
            if (cursor < len) { cursor++; redraw(); }

        } else if (c == 0x1000) {   // Up arrow — history back
            if (s_hist_count == 0) continue;
            if (hist_pos == s_hist_count) {
                tmp[len] = '\0';
                strncpy(saved, tmp, sizeof saved - 1);
            }
            if (hist_pos > 0) hist_pos--;
            int idx = (s_hist_head - s_hist_count + hist_pos + SDL_HIST_MAX) % SDL_HIST_MAX;
            strncpy(tmp, s_hist[idx], sizeof tmp - 1);
            len = cursor = (int)strlen(tmp);
            redraw();

        } else if (c == 0x1001) {   // Down arrow — history forward
            if (hist_pos < s_hist_count) hist_pos++;
            if (hist_pos == s_hist_count) {
                strncpy(tmp, saved, sizeof tmp - 1);
            } else {
                int idx = (s_hist_head - s_hist_count + hist_pos + SDL_HIST_MAX) % SDL_HIST_MAX;
                strncpy(tmp, s_hist[idx], sizeof tmp - 1);
            }
            len = cursor = (int)strlen(tmp);
            redraw();

        } else if (c >= 32 && c < 256) {   // Printable
            if (len < bufsz - 1 && len < (int)sizeof(tmp) - 1) {
                memmove(tmp + cursor + 1, tmp + cursor, len - cursor);
                tmp[cursor++] = (char)c;
                len++;
                redraw();
            }
        }
    }

    tmp[len] = '\0';

    // Add to history if non-empty and different from last entry
    if (len > 0) {
        int last = (s_hist_head - 1 + SDL_HIST_MAX) % SDL_HIST_MAX;
        if (s_hist_count == 0 || strcmp(s_hist[last], tmp) != 0) {
            strncpy(s_hist[s_hist_head], tmp, sizeof s_hist[0] - 1);
            s_hist_head = (s_hist_head + 1) % SDL_HIST_MAX;
            if (s_hist_count < SDL_HIST_MAX) s_hist_count++;
        }
    }

    strncpy(buf, tmp, bufsz - 1);
    buf[bufsz - 1] = '\0';
    s_cursor_vis = false;
    gfx_sdl_render();
    return len;
}

/* ============================================================================
 * Frame rate limiting (60 FPS)
 * ============================================================================ */

void gfx_sdl_frame_start(void)
{
    s_frame_start_time = SDL_GetTicks();
}

void gfx_sdl_frame_end(void)
{
    Uint32 elapsed = SDL_GetTicks() - s_frame_start_time;
    if (elapsed < FRAME_TIME_MS) {
        SDL_Delay(FRAME_TIME_MS - elapsed);
    }
}

BASIC_NS_END

#endif /* USE_SDL_WINDOW */
