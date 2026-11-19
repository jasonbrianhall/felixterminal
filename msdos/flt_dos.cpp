// flt_dos.cpp — Felix Terminal for MS-DOS: the front end.
//
// FelixTerminal's own terminal (terminal.cpp), sixel and kitty graphics
// (sixel_graphics.cpp, kitty_graphics.cpp) and SSH session (ssh_session.cpp,
// libssh2) run unchanged; this file replaces felixterminal.cpp, term_ui.cpp,
// ft_font.cpp and the GL renderer, none of which suit DOS:
//
//   - the screen: SDL3's DOS port (a VESA mode, fullscreen) and its software
//     renderer. The text is drawn into a CPU buffer, only the rows the
//     terminal marks dirty, with FreeType and the embedded DejaVu Sans Mono;
//     that buffer is a streaming texture. Sixel and kitty images are SDL
//     textures, drawn over it by sixel_render()/kitty_render() as on the
//     other systems' SDL path.
//   - the keyboard: SDL key and text events, turned into the sequences
//     handle_key() sends (xterm).
//   - SSH: ssh_connect() runs on the one thread there is; its password and
//     host-key prompts are answered in the terminal itself.
//   - the mouse (INT 33h, through SDL): reported to programs that ask
//     (X10/SGR, as felixterminal sends it); otherwise left-drag selects and
//     copies, middle-click pastes, right-click opens a menu: Copy, Paste and
//     felixterminal's themes (the choice is kept in %HOME%/FLT.CFG).
//
//   FLT [-4|-6] [-p port] [-l user] [-i keyfile] [-g WxH] [-f size] [-t theme]
//       [user@]host[:port] [command]
//
// See msdos/README.md.

#include "terminal.h"
#include "term_pty.h"
#include "term_color.h"
#include "gl_terminal.h"
#include "kitty_graphics.h"
#include "sixel_graphics.h"
#include "ssh_session.h"

#include <ft2build.h>
#include FT_FREETYPE_H

// The fonts, linked in as their bytes (build/fonts.o, from fonts-dos.py and
// ../DejaVuMono.h, ...: the same TTFs, not base64), so FreeType reads them
// where they are instead of from a decoded copy on the heap.
#define FONT(n) extern "C" const unsigned char font_##n[]; extern "C" const unsigned long font_##n##_size;
FONT(dejavumono) FONT(dejavumonobold) FONT(dejavumonooblique) FONT(dejavumonoboldoblique) FONT(freemono)
#undef FONT

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <ctype.h>

// ============================================================================
// GLOBALS THE SHARED FILES EXPECT
// ============================================================================

int           g_font_size        = FONT_SIZE_DEFAULT;
FT_Face       s_ft_face          = nullptr;   // terminal.cpp measures the cell with it
bool          g_use_sdl_renderer = true;      // sixel/kitty: always their SDL path
SDL_Renderer *g_sdl_renderer     = nullptr;
SDL_Window   *g_sdl_window       = nullptr;
int           g_basic_win_w = 0, g_basic_win_h = 0;
const char   *g_term_type        = "xterm-256color";
void (*g_term_write_override)(Terminal *t, const char *s, int n) = nullptr;

void gl_flush_verts(void) {}
// Felix BASIC's graphics (OSC sequences from FELIXBAS): not on DOS.
void basic_handle_osc(Terminal *, const char *, int, int, int) {}

// Replies (cursor position, device attributes, kitty's OK) and keys go to
// the SSH channel (ssh_connect sets the override).
void term_write(Terminal *t, const char *s, int n) {
    if (g_term_write_override) g_term_write_override(t, s, n);
}

// ============================================================================
// FONT
// ============================================================================

static FT_Library s_ft_lib;
static FT_Face    s_face[4];        // regular, bold, oblique, bold oblique
static FT_Face    s_face_fallback;  // FreeMono: symbols DejaVu doesn't have

static FT_Face load_face(const unsigned char *ttf, unsigned long size) {
    FT_Face f = nullptr;
    return FT_New_Memory_Face(s_ft_lib, ttf, (FT_Long)size, 0, &f) == 0 ? f : nullptr;
}

static bool font_init(void) {
    if (FT_Init_FreeType(&s_ft_lib)) return false;
    s_face[0] = load_face(font_dejavumono, font_dejavumono_size);
    s_face[1] = load_face(font_dejavumonobold, font_dejavumonobold_size);
    s_face[2] = load_face(font_dejavumonooblique, font_dejavumonooblique_size);
    s_face[3] = load_face(font_dejavumonoboldoblique, font_dejavumonoboldoblique_size);
    s_face_fallback = load_face(font_freemono, font_freemono_size);
    if (!s_face[0]) return false;
    for (int i = 1; i < 4; i++) if (!s_face[i]) s_face[i] = s_face[0];
    s_ft_face = s_face[0];
    return true;
}

struct Glyph {
    int left, top, w, h;
    std::vector<uint8_t> a;         // 8-bit coverage, w*h
};
static std::unordered_map<uint32_t, Glyph> s_glyphs;

static const Glyph &glyph(uint32_t cp, int style) {
    uint32_t key = cp | (uint32_t)style << 22;
    auto it = s_glyphs.find(key);
    if (it != s_glyphs.end()) return it->second;
    Glyph g = {0, 0, 0, 0, {}};
    FT_Face f = s_face[style];
    if (!FT_Get_Char_Index(f, cp)) {
        if (FT_Get_Char_Index(s_face[0], cp)) f = s_face[0];
        else if (s_face_fallback && FT_Get_Char_Index(s_face_fallback, cp)) f = s_face_fallback;
    }
    FT_Set_Pixel_Sizes(f, 0, g_font_size);
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0) {
        FT_Bitmap &b = f->glyph->bitmap;
        g.left = f->glyph->bitmap_left;
        g.top  = f->glyph->bitmap_top;
        g.w = (int)b.width;
        g.h = (int)b.rows;
        g.a.resize((size_t)g.w * g.h);
        for (int y = 0; y < g.h; y++)
            memcpy(&g.a[(size_t)y * g.w], b.buffer + y * b.pitch, g.w);
    }
    return s_glyphs.emplace(key, std::move(g)).first->second;
}

// ============================================================================
// SCREEN
// ============================================================================

static Terminal     s_term;
// The text layer, XRGB8888, s_w*s_h: drawn straight into the texture's own
// pixels (text_lock()), with no copy of the screen beside it. SDL's software
// renderer keeps a streaming texture's pixels between locks (its lock hands
// out the surface it draws from), so only the dirty rows are redrawn.
static SDL_Texture *s_text_tex;
static int          s_w, s_h;           // the screen
static int          s_ox = 2, s_oy = 2; // the grid's top-left (2 px, as felixterminal)
static int          s_cw, s_ch;         // the cell

// Where fill() and draw_glyph() draw: the text layer, or the menu's buffer.
static uint32_t *s_px;
static int       s_pw, s_ph, s_ps;      // width, height, pixels from one row to the next
static void draw_into(std::vector<uint32_t> &buf, int w, int h) { s_px = buf.data(); s_pw = w; s_ph = h; s_ps = w; }
static bool text_lock(void) {
    void *p; int pitch;
    if (!SDL_LockTexture(s_text_tex, nullptr, &p, &pitch)) return false;
    s_px = (uint32_t *)p; s_pw = s_w; s_ph = s_h; s_ps = pitch / 4;
    return true;
}
static void text_unlock(void) { SDL_UnlockTexture(s_text_tex); }
static void text_clear(uint32_t c) {
    if (!text_lock()) return;
    for (int y = 0; y < s_h; y++) std::fill(s_px + (size_t)y * s_ps, s_px + (size_t)y * s_ps + s_w, c);
    text_unlock();
}

static inline uint32_t rgb(TermColor c) {
    int r = (int)(c.r * 255.f + .5f), g = (int)(c.g * 255.f + .5f), b = (int)(c.b * 255.f + .5f);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static inline uint32_t blend(uint32_t bg, uint32_t fg, unsigned a) {
    if (a >= 255) return fg;
    if (!a) return bg;
    unsigned b = 255 - a;            // each channel on its own: one division of
    uint32_t r = ((fg >> 16 & 255) * a + (bg >> 16 & 255) * b) / 255;   // packed channels
    uint32_t g = ((fg >> 8 & 255) * a + (bg >> 8 & 255) * b) / 255;     // leaks between them
    uint32_t l = ((fg & 255) * a + (bg & 255) * b) / 255;
    return r << 16 | g << 8 | l;
}

static void fill(int x, int y, int w, int h, uint32_t c, unsigned a = 255) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_pw) w = s_pw - x;
    if (y + h > s_ph) h = s_ph - y;
    for (int j = 0; j < h; j++) {
        uint32_t *p = &s_px[(size_t)(y + j) * s_ps + x];
        if (a >= 255) for (int i = 0; i < w; i++) p[i] = c;
        else for (int i = 0; i < w; i++) p[i] = blend(p[i], c, a);
    }
}

// Block elements (U+2580-259F) as exact rectangles, as term_draw_block_element
// does: timg's and btop's half blocks then meet with no seams.
static bool block_element(uint32_t cp, int px, int py, uint32_t fc) {
    if (cp < 0x2580 || cp > 0x259F) return false;
    int w = s_cw, h = s_ch;
    switch (cp) {
    case 0x2580: fill(px, py, w, h / 2, fc); return true;
    case 0x2588: fill(px, py, w, h, fc); return true;
    case 0x2590: fill(px + w / 2, py, w - w / 2, h, fc); return true;
    case 0x2591: fill(px, py, w, h, fc, 64); return true;
    case 0x2592: fill(px, py, w, h, fc, 128); return true;
    case 0x2593: fill(px, py, w, h, fc, 192); return true;
    case 0x2594: fill(px, py, w, (h + 7) / 8, fc); return true;
    case 0x2595: { int e = (w + 7) / 8; fill(px + w - e, py, e, h, fc); return true; }
    default: break;
    }
    if (cp >= 0x2581 && cp <= 0x2587) {               // lower 1/8 .. 7/8
        int e = h * (int)(cp - 0x2580) / 8;
        fill(px, py + h - e, w, e, fc);
        return true;
    }
    if (cp >= 0x2589 && cp <= 0x258F) {               // left 7/8 .. 1/8
        int e = w * (int)(0x2590 - cp) / 8;
        fill(px, py, e, h, fc);
        return true;
    }
    // quadrants 2596-259F: bits UL, UR, LL, LR
    static const uint8_t q[10] = { 4, 8, 1, 1|4|8, 1|8, 1|2|4, 1|2|8, 2, 2|4, 2|4|8 };
    uint8_t m = q[cp - 0x2596];
    int hw = w / 2, hh = h / 2;
    if (m & 1) fill(px, py, hw, hh, fc);
    if (m & 2) fill(px + hw, py, w - hw, hh, fc);
    if (m & 4) fill(px, py + hh, hw, h - hh, fc);
    if (m & 8) fill(px + hw, py + hh, w - hw, h - hh, fc);
    return true;
}

static void draw_glyph(uint32_t cp, int style, int px, int py, uint32_t fc) {
    const Glyph &g = glyph(cp, style);
    int baseline = py + (int)(s_ch * 0.82f);
    int x0 = px + g.left, y0 = baseline - g.top;
    for (int y = 0; y < g.h; y++) {
        int sy = y0 + y;
        if (sy < py || sy >= py + s_ch || sy < 0 || sy >= s_ph) continue;   // stays in its row
        uint32_t *row = &s_px[(size_t)sy * s_ps];
        const uint8_t *a = &g.a[(size_t)y * g.w];
        for (int x = 0; x < g.w; x++) {
            int sx = x0 + x;
            if (a[x] && sx >= 0 && sx < s_pw) row[sx] = blend(row[sx], fc, a[x]);
        }
    }
}

// What's shown at screen row 'row' (the scrollback when scrolled back).
static Cell s_blank = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0, 0, 0}};
static Cell *shown_cell(Terminal *t, int row, int col) {
    if (t->sb_offset > 0) {
        int sb = t->sb_count - t->sb_offset + row;
        if (sb < 0) return &s_blank;
        if (sb < t->sb_count) return sb_row(t, sb) + col;
        int live = sb - t->sb_count;
        return live < t->rows ? &CELL(t, live, col) : &s_blank;
    }
    return &CELL(t, row, col);
}

// ============================================================================
// SELECTION (in virtual rows: the scrollback's, then the screen's)
// ============================================================================

static struct { bool on, drag; int r0, c0, r1, c1; } s_sel;
static std::string s_clip;            // DOS has no clipboard: FLT's own

static inline int vrow_of(Terminal *t, int row) { return row + t->sb_count - t->sb_offset; }

static bool in_sel(int vrow, int col) {
    if (!s_sel.on) return false;
    int r0 = s_sel.r0, c0 = s_sel.c0, r1 = s_sel.r1, c1 = s_sel.c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    if (vrow < r0 || vrow > r1) return false;
    if (vrow == r0 && col < c0) return false;
    if (vrow == r1 && col > c1) return false;
    return true;
}

static Cell *vcell_at(Terminal *t, int vrow, int col) {
    if (vrow < 0) return &s_blank;
    if (vrow < t->sb_count) return col < t->sb_cols ? sb_row(t, vrow) + col : &s_blank;
    vrow -= t->sb_count;
    return vrow < t->rows && col < t->cols ? &CELL(t, vrow, col) : &s_blank;
}

static void put_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
    else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 63)); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
}

// The selection as text: UTF-8, rows without their trailing blanks, \n between.
static std::string sel_text(Terminal *t) {
    std::string out;
    if (!s_sel.on) return out;
    int r0 = s_sel.r0, c0 = s_sel.c0, r1 = s_sel.r1, c1 = s_sel.c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    for (int r = r0; r <= r1; r++) {
        std::string line;
        int a = r == r0 ? c0 : 0, b = r == r1 ? c1 : t->cols - 1;
        for (int c = a; c <= b; c++) {
            Cell *cl = vcell_at(t, r, c);
            if (cell_is_wide_tail(cl)) continue;
            put_utf8(line, cl->cp ? cl->cp : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        if (r < r1) out += '\n';
    }
    return out;
}

static void draw_row(Terminal *t, int row) {
    int py = s_oy + row * s_ch;
    int vrow = vrow_of(t, row);
    for (int col = 0; col < t->cols; col++) {
        int px = s_ox + col * s_cw;
        Cell *c = shown_cell(t, row, col);
        TermColorVal fg = c->fg, bg = c->bg;
        if (c->attrs & ATTR_REVERSE) { TermColorVal x = fg; fg = bg; bg = x; }
        uint32_t bc = rgb(tcolor_resolve(bg));
        TermColor f = tcolor_resolve(fg);
        if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
            f = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg) + 8));
        if (c->attrs & ATTR_DIM) { f.r *= .5f; f.g *= .5f; f.b *= .5f; }
        uint32_t fc = rgb(f);
        if (in_sel(vrow, col)) {                           // as felixterminal: the colors swapped
            uint32_t x = rgb(tcolor_resolve(fg));
            fc = bc; bc = x;
        }
        int cells = cell_is_wide(c) ? 2 : 1;
        if (cell_is_wide_tail(c)) continue;                // drawn with its left half
        fill(px, py, s_cw * cells, s_ch, bc);
        if (cell_is_hidden(c)) continue;
        uint32_t cp = c->cp;
        if (cp && cp != ' ' && !block_element(cp, px, py, fc)) {
            int style = ((c->attrs & ATTR_BOLD) ? 1 : 0) | ((c->attrs & ATTR_ITALIC) ? 2 : 0);
            draw_glyph(cp, style, px, py, fc);
        }
        int w = s_cw * cells;
        if (c->attrs & ATTR_UNDERLINE) fill(px, py + s_ch - 2, w, 1, fc);
        if (c->attrs & ATTR_STRIKE)    fill(px, py + s_ch / 2, w, 1, fc);
        if (c->attrs & ATTR_OVERLINE)  fill(px, py, w, 1, fc);
    }
}

// ============================================================================
// MENU (right-click): Copy, Paste, the themes
// ============================================================================

enum { M_NONE, M_COPY, M_PASTE, M_THEMES, M_MODES, M_BACK, M_THEME0 = 100, M_MODE0 = 200 };
enum { PAGE_MAIN, PAGE_THEMES, PAGE_MODES };
struct MenuItem { std::string label; int action; };
static std::vector<SDL_DisplayMode> s_modes;   // a mode per size, the most colors
static SDL_DisplayMode s_mode;                 // the one in use
static struct {
    bool open, armed;                          // armed: the click that opened it is over
    int page, ax, ay;                          // ax, ay: where it was opened
    int x, y, w, h, hover;
    std::vector<MenuItem> items;
    std::vector<uint32_t> px;
    SDL_Texture *tex;
    int tex_w, tex_h;
} s_menu;

static std::string mode_name(const SDL_DisplayMode &m) {
    char b[48];
    snprintf(b, sizeof b, "%dx%d, %d bits", m.w, m.h, SDL_BYTESPERPIXEL(m.format) == 4 ? 32 : SDL_BITSPERPIXEL(m.format));
    return b;
}

static void menu_open(int mx, int my, int page = PAGE_MAIN) {
    s_menu.items.clear();
    s_menu.page = page;
    s_menu.ax = mx; s_menu.ay = my;
    if (page == PAGE_MAIN) {
        s_menu.items.push_back({ "Copy", s_sel.on ? M_COPY : M_NONE });
        s_menu.items.push_back({ "Paste", s_clip.empty() ? M_NONE : M_PASTE });
        s_menu.items.push_back({ "", M_NONE });
        s_menu.items.push_back({ "Theme...", M_THEMES });
        s_menu.items.push_back({ "Resolution...", M_MODES });
    } else if (page == PAGE_THEMES) {
        s_menu.items.push_back({ "< Back", M_BACK });
        s_menu.items.push_back({ "", M_NONE });
        for (int i = 0; i < THEME_COUNT; i++)
            s_menu.items.push_back({ std::string(i == g_theme_idx ? "* " : "  ") + THEMES[i].name, M_THEME0 + i });
    } else {
        s_menu.items.push_back({ "< Back", M_BACK });
        s_menu.items.push_back({ "", M_NONE });
        for (size_t i = 0; i < s_modes.size(); i++) {
            bool cur = s_modes[i].w == s_mode.w && s_modes[i].h == s_mode.h;
            s_menu.items.push_back({ std::string(cur ? "* " : "  ") + mode_name(s_modes[i]), M_MODE0 + (int)i });
        }
    }
    size_t longest = 0;
    for (auto &it : s_menu.items) longest = std::max(longest, it.label.size());
    s_menu.w = (int)(longest + 4) * s_cw;
    s_menu.h = (int)s_menu.items.size() * s_ch + 4;
    s_menu.x = std::min(mx, s_w - s_menu.w);
    s_menu.y = std::min(my, s_h - s_menu.h);
    if (s_menu.x < 0) s_menu.x = 0;
    if (s_menu.y < 0) s_menu.y = 0;
    s_menu.hover = -1;
    if (!s_menu.open) s_menu.armed = false;    // (a new page: armed as it was)
    s_menu.open = true;
}

static int menu_hit(int mx, int my) {
    if (mx < s_menu.x || mx >= s_menu.x + s_menu.w || my < s_menu.y + 2 || my >= s_menu.y + s_menu.h - 2) return -1;
    return (my - s_menu.y - 2) / s_ch;
}

static void menu_draw(void) {
    int w = s_menu.w, h = s_menu.h;
    s_menu.px.assign((size_t)w * h, 0);
    draw_into(s_menu.px, w, h);
    uint32_t bg = rgb(tcolor_resolve(TCOLOR_PALETTE(0)));
    uint32_t fg = rgb(tcolor_resolve(TCOLOR_PALETTE(7)));
    uint32_t dim = rgb(tcolor_resolve(TCOLOR_PALETTE(8)));
    uint32_t hi = rgb(tcolor_resolve(TCOLOR_PALETTE(4)));
    fill(0, 0, w, h, fg);                                  // a 1-pixel frame
    fill(1, 1, w - 2, h - 2, bg);
    for (size_t i = 0; i < s_menu.items.size(); i++) {
        const MenuItem &it = s_menu.items[i];
        int y = 2 + (int)i * s_ch;
        if (it.label.empty()) { fill(s_cw, y + s_ch / 2, w - 2 * s_cw, 1, dim); continue; }
        bool active = it.action != M_NONE;
        if (active && (int)i == s_menu.hover) fill(2, y, w - 4, s_ch, hi);
        uint32_t c = active ? fg : dim;
        for (size_t k = 0; k < it.label.size(); k++)
            if (it.label[k] != ' ') draw_glyph((unsigned char)it.label[k], 0, (int)(k + 2) * s_cw, y, c);
    }
    if (!s_menu.tex || s_menu.tex_w != w || s_menu.tex_h != h) {
        if (s_menu.tex) SDL_DestroyTexture(s_menu.tex);
        s_menu.tex = SDL_CreateTexture(g_sdl_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        s_menu.tex_w = w; s_menu.tex_h = h;
    }
    SDL_UpdateTexture(s_menu.tex, nullptr, s_menu.px.data(), w * 4);
    SDL_FRect d = { (float)s_menu.x, (float)s_menu.y, (float)w, (float)h };
    SDL_RenderTexture(g_sdl_renderer, s_menu.tex, nullptr, &d);
}

// The text layer's dirty rows into the texture, then the frame: text,
// images, cursor, the menu.
static void render(Terminal *t) {
    if (text_lock()) {
        for (int r = 0; r < t->rows; r++)
            if (term_row_is_dirty(t, r)) draw_row(t, r);
        text_unlock();
        term_clear_dirty(t);
    }
    SDL_RenderTexture(g_sdl_renderer, s_text_tex, nullptr, nullptr);
    kitty_render(t, s_ox, s_oy);
    sixel_render(t, s_ox, s_oy);
    if (t->sb_offset == 0 && term_cursor_visible(t)) {
        SDL_FRect r = { (float)(s_ox + t->cur_col * s_cw), (float)(s_oy + t->cur_row * s_ch),
                        (float)s_cw, (float)s_ch };
        SDL_SetRenderDrawBlendMode(g_sdl_renderer, SDL_BLENDMODE_BLEND);
        if (t->cursor_shape == 0) {
            SDL_SetRenderDrawColor(g_sdl_renderer, 255, 255, 255, 110);
        } else if (t->cursor_shape == 2) {
            r.w = 2;
            SDL_SetRenderDrawColor(g_sdl_renderer, 255, 255, 255, 220);
        } else {
            r.y += r.h - 3; r.h = 3;
            SDL_SetRenderDrawColor(g_sdl_renderer, 255, 255, 255, 220);
        }
        SDL_RenderFillRect(g_sdl_renderer, &r);
    }
    if (s_menu.open) menu_draw();
    SDL_RenderPresent(g_sdl_renderer);
}

// The VESA mode: -g / FLT_VIDEO (WIDTHxHEIGHT[xBITS]), else 1024x768, else
// the largest of at least 640x480. 32-bit first, then 16-bit.
// The menu's resolutions: every size of 640x400 or more, each in its most
// colors (4 bytes a pixel, then 16, then 15 bits), smallest first.
static void list_modes(void) {
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
    if (!modes) return;
    auto rank = [](const SDL_DisplayMode *m) {
        int bpp = SDL_BITSPERPIXEL(m->format);
        return SDL_BYTESPERPIXEL(m->format) == 4 ? 3 : bpp == 16 ? 2 : bpp == 15 ? 1 : 0;
    };
    s_modes.clear();
    for (int i = 0; i < n; i++) {
        const SDL_DisplayMode *m = modes[i];
        if (SDL_ISPIXELFORMAT_INDEXED(m->format) || SDL_BITSPERPIXEL(m->format) < 15 || m->w < 640 || m->h < 400) continue;
        bool done = false;
        for (auto &k : s_modes)
            if (k.w == m->w && k.h == m->h) { if (rank(m) > rank(&k)) k = *m; done = true; }
        if (!done) s_modes.push_back(*m);
    }
    SDL_free(modes);
    std::sort(s_modes.begin(), s_modes.end(), [](const SDL_DisplayMode &a, const SDL_DisplayMode &b) {
        return a.w * a.h != b.w * b.h ? a.w * a.h < b.w * b.h : a.w < b.w; });
}

static bool pick_mode(int want_w, int want_h, int want_bpp, SDL_DisplayMode *out) {
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
    if (!modes) return false;
    const SDL_DisplayMode *best = nullptr;
    long best_score = -1;
    for (int i = 0; i < n; i++) {
        const SDL_DisplayMode *m = modes[i];
        int bpp = SDL_BITSPERPIXEL(m->format);
        if (SDL_ISPIXELFORMAT_INDEXED(m->format) || bpp < 15 || m->w < 640 || m->h < 400) continue;
        if (want_bpp && bpp != want_bpp && SDL_BYTESPERPIXEL(m->format) * 8 != want_bpp) continue;
        long score;
        if (want_w) score = (m->w == want_w && m->h == want_h) ? 1000000 : 0;
        else score = (m->w == 1024 && m->h == 768) ? 1000000 : m->w * m->h / 4;
        // XRGB8888 says 24 bits a pixel: rank by bytes (4: true color)
        int bytes = SDL_BYTESPERPIXEL(m->format);
        score = score * 4 + (bytes == 4 ? 3 : bpp == 16 ? 2 : bpp == 15 ? 1 : 0);
        if (score > best_score) { best_score = score; best = m; }
    }
    if (best) *out = *best;          // (the list is freed here)
    SDL_free(modes);
    return best != nullptr;
}

// ============================================================================
// KEYBOARD (as handle_key in term_ui.cpp)
// ============================================================================

static void paste(Terminal *t);

static void send(const char *s, int n = -1) {
    term_write(&s_term, s, n < 0 ? (int)strlen(s) : n);
}

// A key press. Returns true when it was handled here (no text to send).
static bool key_down(Terminal *t, SDL_Keycode k, SDL_Keymod mod) {
    bool shift = mod & SDL_KMOD_SHIFT, ctrl = mod & SDL_KMOD_CTRL, alt = mod & SDL_KMOD_ALT;

    // Shift+PgUp/PgDn/Home/End: the scrollback, as on the other systems
    if (shift && !ctrl && !alt && (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN || k == SDLK_HOME || k == SDLK_END)) {
        int o = t->sb_offset;
        if (k == SDLK_PAGEUP) o += t->rows / 2;
        else if (k == SDLK_PAGEDOWN) o -= t->rows / 2;
        else if (k == SDLK_HOME) o = t->sb_count;
        else o = 0;
        if (o < 0) o = 0;
        if (o > t->sb_count) o = t->sb_count;
        if (o != t->sb_offset) { t->sb_offset = o; term_dirty_all(t); }
        return true;
    }
    if (t->sb_offset && k != SDLK_LSHIFT && k != SDLK_RSHIFT) { t->sb_offset = 0; term_dirty_all(t); }

    auto arrow = [&](const char *normal, const char *app, char letter) {
        if (!shift && !ctrl && !alt) send(t->app_cursor_keys ? app : normal, 3);
        else {
            char seq[16];
            int n = snprintf(seq, sizeof seq, "\x1b[1;%d%c", 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0), letter);
            send(seq, n);
        }
    };
    switch (k) {
    case SDLK_UP:    arrow("\x1b[A", "\x1bOA", 'A'); return true;
    case SDLK_DOWN:  arrow("\x1b[B", "\x1bOB", 'B'); return true;
    case SDLK_RIGHT: arrow("\x1b[C", "\x1bOC", 'C'); return true;
    case SDLK_LEFT:  arrow("\x1b[D", "\x1bOD", 'D'); return true;
    case SDLK_HOME:  send(t->app_cursor_keys ? "\x1bOH" : "\x1b[H", 3); return true;
    case SDLK_END:   send(t->app_cursor_keys ? "\x1bOF" : "\x1b[F", 3); return true;
    case SDLK_RETURN: case SDLK_KP_ENTER: send("\r", 1); return true;
    case SDLK_BACKSPACE: send("\x7f", 1); return true;
    case SDLK_TAB:   send(shift ? "\x1b[Z" : "\t"); return true;
    case SDLK_ESCAPE: send("\x1b", 1); return true;
    case SDLK_INSERT: if (shift) paste(t); else send("\x1b[2~"); return true;
    case SDLK_DELETE: send("\x1b[3~"); return true;
    case SDLK_PAGEUP: send("\x1b[5~"); return true;
    case SDLK_PAGEDOWN: send("\x1b[6~"); return true;
    case SDLK_F1:  send("\x1bOP"); return true;
    case SDLK_F2:  send("\x1bOQ"); return true;
    case SDLK_F3:  send("\x1bOR"); return true;
    case SDLK_F4:  send("\x1bOS"); return true;
    case SDLK_F5:  send("\x1b[15~"); return true;
    case SDLK_F6:  send("\x1b[17~"); return true;
    case SDLK_F7:  send("\x1b[18~"); return true;
    case SDLK_F8:  send("\x1b[19~"); return true;
    case SDLK_F9:  send("\x1b[20~"); return true;
    case SDLK_F10: send("\x1b[21~"); return true;
    case SDLK_F11: send("\x1b[23~"); return true;
    case SDLK_F12: send("\x1b[24~"); return true;
    default: break;
    }
    // Ctrl and Alt combinations: the DOS port sends no text for them.
    if (ctrl && k >= SDLK_A && k <= SDLK_Z) {
        char c = (char)(k - SDLK_A + 1);
        if (alt) send("\x1b", 1);
        send(&c, 1);
        return true;
    }
    if (ctrl) {
        char c = 0;
        switch (k) {
        case SDLK_SPACE: case SDLK_2: c = 0; send(&c, 1); return true;
        case SDLK_LEFTBRACKET: case SDLK_3: c = 0x1b; break;
        case SDLK_BACKSLASH: case SDLK_4: c = 0x1c; break;
        case SDLK_RIGHTBRACKET: case SDLK_5: c = 0x1d; break;
        case SDLK_6: c = 0x1e; break;
        case SDLK_MINUS: case SDLK_7: c = 0x1f; break;
        default: break;
        }
        if (c) { send(&c, 1); return true; }
    }
    if (alt && k > 0 && k < 0x80 && !ctrl) {
        char c = (char)k;
        if (shift && c >= 'a' && c <= 'z') c -= 32;
        send("\x1b", 1);
        send(&c, 1);
        return true;
    }
    return false;
}

// ============================================================================
// THEMES (felixterminal's): -t, FLT_THEME, the menu; kept in %HOME%/FLT.CFG
// ============================================================================

static void repaint_all(Terminal *t) {               // after a theme change
    text_clear(rgb(tcolor_resolve(TCOLOR_PALETTE(0))));
    term_dirty_all(t);
}

// A theme by name (case and spaces ignored, a prefix will do) or number (1-).
static int theme_find(const char *want) {
    auto norm = [](const char *s) { std::string o; for (; *s; s++) if (*s != ' ' && *s != '_' && *s != '-') o += (char)tolower((unsigned char)*s); return o; };
    std::string w = norm(want);
    if (w.empty()) return -1;
    if (isdigit((unsigned char)w[0])) { int n = atoi(w.c_str()); return n >= 1 && n <= THEME_COUNT ? n - 1 : -1; }
    for (int i = 0; i < THEME_COUNT; i++) if (norm(THEMES[i].name) == w) return i;
    for (int i = 0; i < THEME_COUNT; i++) if (norm(THEMES[i].name).compare(0, w.size(), w) == 0) return i;
    return -1;
}

static std::string cfg_path(void) {
    const char *h = getenv("HOME");
    return std::string(h && *h ? h : "C:/HOME") + "/FLT.CFG";
}
// FLT.CFG: theme=NAME and mode=WIDTHxHEIGHTxBITS, as the menu last set them.
static int cfg_load(int *w, int *h, int *bpp) {
    FILE *f = fopen(cfg_path().c_str(), "r");
    if (!f) return -1;
    char line[128];
    int idx = -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "theme=", 6)) idx = theme_find(line + 6);
        else if (!strncmp(line, "mode=", 5) && w && !*w) { *bpp = 0; sscanf(line + 5, "%dx%dx%d", w, h, bpp); }
    }
    fclose(f);
    return idx;
}
static void cfg_save(void) {
    const char *h = getenv("HOME");
    mkdir(h && *h ? h : "C:/HOME", 0755);              // (C:\HOME, as vmdos's ssh, may not be there yet)
    FILE *f = fopen(cfg_path().c_str(), "w");
    if (!f) return;
    fprintf(f, "theme=%s\nmode=%dx%dx%d\n", THEMES[g_theme_idx].name, s_mode.w, s_mode.h,
            SDL_BYTESPERPIXEL(s_mode.format) == 4 ? 32 : SDL_BITSPERPIXEL(s_mode.format));
    fclose(f);
}

// Another VESA mode while running (the menu's Resolution): the screen, the
// text layer and the grid follow, and the server is told the new size.
static void new_text_layer(void) {
    SDL_GetCurrentRenderOutputSize(g_sdl_renderer, &s_w, &s_h);
    if (s_text_tex) SDL_DestroyTexture(s_text_tex);
    s_text_tex = SDL_CreateTexture(g_sdl_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, s_w, s_h);
    text_clear(rgb(tcolor_resolve(TCOLOR_PALETTE(0))));
}
static void set_mode(Terminal *t, const SDL_DisplayMode &m) {
    if (m.w == s_mode.w && m.h == s_mode.h && m.format == s_mode.format) return;
    // SDL's DOS port switches the VESA mode when a window is resized, and SDL
    // passes no resize to it while the window is fullscreen: out of it, the
    // size (the mode), the exact mode (its colors), and back in.
    SDL_DisplayMode want = m;
    SDL_SetWindowFullscreen(g_sdl_window, false);
    SDL_SyncWindow(g_sdl_window);
    SDL_SetWindowSize(g_sdl_window, m.w, m.h);
    SDL_SyncWindow(g_sdl_window);
    SDL_SetWindowFullscreenMode(g_sdl_window, &want);
    SDL_SetWindowFullscreen(g_sdl_window, true);
    SDL_SyncWindow(g_sdl_window);
    SDL_PumpEvents();                                  // the renderer sees the new window size
    s_mode = m;
    new_text_layer();
    s_sel.on = s_sel.drag = false;
    term_resize(t, s_w, s_h);
    term_dirty_all(t);
    ssh_pty_resize(t->cols, t->rows);                 // (only while connected)
}

// ============================================================================
// MOUSE
// ============================================================================

static void paste(Terminal *t) {
    if (s_clip.empty()) return;
    std::string p;
    for (char c : s_clip) p += c == '\n' ? '\r' : c;
    if (t->bracketed_paste) p = "\x1b[200~" + p + "\x1b[201~";
    send(p.c_str(), (int)p.size());
}

static void cell_at(Terminal *t, float x, float y, int *row, int *col) {
    int c = ((int)x - s_ox) / s_cw, r = ((int)y - s_oy) / s_ch;
    *col = c < 0 ? 0 : c >= t->cols ? t->cols - 1 : c;
    *row = r < 0 ? 0 : r >= t->rows ? t->rows - 1 : r;
}

// SET FLT_LOG=file: what the mouse did, for when it doesn't
static FILE *s_log;
static void dlog(const char *fmt, ...) {
    if (!s_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(s_log, fmt, ap); va_end(ap);
    fflush(s_log);
    fsync(fileno(s_log));                              // (DOS: the size is on disk only once committed)
}

// A press, release or move; true when the screen needs drawing again.
static bool mouse_event(Terminal *t, const SDL_Event &e) {
    if (e.type != SDL_EVENT_MOUSE_MOTION)
        dlog("mouse %s button %d at %.0f,%.0f (menu %d)\n", e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up",
             e.button.button, e.button.x, e.button.y, s_menu.open);
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        if (s_menu.open) { s_menu.hover = menu_hit((int)e.motion.x, (int)e.motion.y); return true; }
        if (s_sel.drag) {
            int r, c;
            cell_at(t, e.motion.x, e.motion.y, &r, &c);
            s_sel.r1 = vrow_of(t, r); s_sel.c1 = c; s_sel.on = true;
            term_dirty_all(t);
        }
        return true;                                       // the pointer moved
    }
    bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    int b = e.button.button;
    int r, c;
    cell_at(t, e.button.x, e.button.y, &r, &c);
    if (s_menu.open) {
        if (down) return true;
        int i = menu_hit((int)e.button.x, (int)e.button.y);
        int act = i >= 0 && i < (int)s_menu.items.size() ? s_menu.items[i].action : M_NONE;
        if (!s_menu.armed) {                           // the release of the click that opened it:
            s_menu.armed = true;                       // it stays open, unless dragged onto an item
            if (act == M_NONE) return true;
        }
        if (act == M_THEMES || act == M_MODES || act == M_BACK) {   // another page of it
            menu_open(s_menu.ax, s_menu.ay, act == M_THEMES ? PAGE_THEMES : act == M_MODES ? PAGE_MODES : PAGE_MAIN);
            return true;
        }
        if (i < 0 || act != M_NONE) s_menu.open = false;   // outside, or a choice: closed
        if (act == M_COPY) s_clip = sel_text(t);
        else if (act == M_PASTE) paste(t);
        else if (act >= M_MODE0) { set_mode(t, s_modes[act - M_MODE0]); cfg_save(); }
        else if (act >= M_THEME0) { apply_theme(act - M_THEME0); cfg_save(); repaint_all(t); }
        return true;
    }
    // The program asked for the mouse (vim, htop, mc): it gets the clicks,
    // as felixterminal sends them, unless Shift is held (then: selection).
    if (t->mouse_report && !(SDL_GetModState() & SDL_KMOD_SHIFT)) {
        int btn = b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_MIDDLE ? 1 : b == SDL_BUTTON_RIGHT ? 2 : -1;
        if (btn < 0) return false;
        char seq[32];
        int n;
        if (t->mouse_sgr) n = snprintf(seq, sizeof seq, "\x1b[<%d;%d;%d%c", btn, c + 1, r + 1, down ? 'M' : 'm');
        else n = snprintf(seq, sizeof seq, "\x1b[M%c%c%c", (char)(32 + (down ? btn : 3)),
                          (char)(32 + (c + 1 > 223 ? 223 : c + 1)), (char)(32 + (r + 1 > 223 ? 223 : r + 1)));
        send(seq, n);
        return false;
    }
    if (b == SDL_BUTTON_LEFT) {
        if (down) {
            if (s_sel.on) term_dirty_all(t);
            s_sel.on = false; s_sel.drag = true;
            s_sel.r0 = s_sel.r1 = vrow_of(t, r); s_sel.c0 = s_sel.c1 = c;
        } else if (s_sel.drag) {
            s_sel.drag = false;
            if (s_sel.on) s_clip = sel_text(t);             // copied on release, as felixterminal
        }
        return true;
    }
    if (b == SDL_BUTTON_MIDDLE && down) { paste(t); return false; }
    if (b == SDL_BUTTON_RIGHT && down) { menu_open((int)e.button.x, (int)e.button.y); return true; }
    return false;
}

// ============================================================================
// PROMPTS (ssh_connect's password and host key questions; the host to use)
// ============================================================================

static bool s_quit;

static std::string prompt_line(const char *prompt, bool secret) {
    term_feed(&s_term, prompt, (int)strlen(prompt));
    std::string s;
    bool need = true;
    uint64_t last_blink = SDL_GetTicks();
    for (;;) {
        if (need) { render(&s_term); need = false; }   // only on a change: the mouse is polled meanwhile
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type != SDL_EVENT_MOUSE_MOTION) dlog("prompt event %x\n", e.type);
            if (e.type == SDL_EVENT_QUIT) { s_quit = true; return ""; }
            need = true;
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_UP) {         // the menu works here too
                mouse_event(&s_term, e);
                continue;
            }
            if (s_menu.open) {                                 // keys: Esc closes the menu
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) s_menu.open = false;
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { term_feed(&s_term, "\r\n", 2); return s; }
                if (k == SDLK_ESCAPE || ((e.key.mod & SDL_KMOD_CTRL) && k == SDLK_C)) {
                    term_feed(&s_term, "\r\n", 2);
                    return "";
                }
                if (k == SDLK_BACKSPACE && !s.empty()) {
                    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
                    if (!s.empty()) s.pop_back();
                    if (!secret) term_feed(&s_term, "\b \b", 3);
                }
            } else if (e.type == SDL_EVENT_TEXT_INPUT) {
                s += e.text.text;
                if (!secret) term_feed(&s_term, e.text.text, (int)strlen(e.text.text));
            }
        }
        uint64_t now = SDL_GetTicks();
        if (s_term.cursor_blink_enabled && now - last_blink >= 500) {
            s_term.cursor_blink_phase = !s_term.cursor_blink_phase;
            last_blink = now;
            need = true;
        }
        if (!need) SDL_Delay(5);
    }
}

// ============================================================================
// MAIN
// ============================================================================

static void usage(void) {
    printf("Felix Terminal for DOS: SSH with sixel and kitty graphics\n\n"
           "  FLT [options] [user@]host[:port] [command]\n\n"
           "  -4, -6         IPv4 or IPv6 only (else as vmdos orders them: IPv6\n"
           "                 first when this PC has a global IPv6 address)\n"
           "  -p PORT        port (default 22)\n"
           "  -l USER        user name\n"
           "  -i KEYFILE     private key (default: ~/.ssh/id_ed25519, id_rsa, ...)\n"
           "  -g WxH[xBITS]  screen mode (default 1024x768; also SET FLT_VIDEO=)\n"
           "  -f SIZE        font size in pixels (default %d)\n"
           "  -t THEME       color theme, by name or number (also SET FLT_THEME=)\n\n"
           "~ is %%HOME%% (C:\\HOME if not set). Shift+PgUp/PgDn scroll back.\n"
           "Mouse: drag selects and copies, middle button or Shift+Ins pastes, right\n"
           "button: menu (copy, paste, themes; the theme is kept in ~/FLT.CFG).\n"
           "TERM is xterm-256color: use timg -pk (kitty) or timg -ps (sixel).\n",
           FONT_SIZE_DEFAULT);
}

int main(int argc, char **argv) {
    SshConfig cfg;
    std::string target;
    int want_w = 0, want_h = 0, want_bpp = 0;
    const char *theme_arg = getenv("FLT_THEME");
    const char *env = getenv("FLT_VIDEO");
    if (env) sscanf(env, "%dx%dx%d", &want_w, &want_h, &want_bpp);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-p") && i + 1 < argc) cfg.port = atoi(argv[++i]);
        else if (!strcmp(a, "-l") && i + 1 < argc) cfg.user = argv[++i];
        else if (!strcmp(a, "-i") && i + 1 < argc) cfg.key_path = argv[++i];
        else if (!strcmp(a, "-g") && i + 1 < argc) { want_bpp = 0; sscanf(argv[++i], "%dx%dx%d", &want_w, &want_h, &want_bpp); }
        else if (!strcmp(a, "-f") && i + 1 < argc) g_font_size = atoi(argv[++i]);
        else if (!strcmp(a, "-t") && i + 1 < argc) theme_arg = argv[++i];
        else if (!strcmp(a, "-4")) cfg.family = 4;
        else if (!strcmp(a, "-6")) cfg.family = 6;
        else if (!strcmp(a, "-?") || !strcmp(a, "/?") || !strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "FLT: unknown option %s (FLT -? for help)\n", a); return 1; }
        else if (target.empty()) target = a;
        else { if (!cfg.command.empty()) cfg.command += ' '; cfg.command += a; }
    }
    if (g_font_size < FONT_SIZE_MIN) g_font_size = FONT_SIZE_MIN;
    if (g_font_size > FONT_SIZE_MAX) g_font_size = FONT_SIZE_MAX;
    if (!getenv("HOME")) setenv("HOME", "C:/HOME", 1);   // as vmdos's ssh
    if (getenv("FLT_LOG")) s_log = fopen(getenv("FLT_LOG"), "w");
    dlog("FLT started\n");
    int theme = cfg_load(&want_w, &want_h, &want_bpp);  // (-g / FLT_VIDEO before FLT.CFG's mode)
    if (theme_arg) theme = theme_find(theme_arg);
    if (theme_arg && theme < 0) {
        fprintf(stderr, "FLT: no theme %s; there are:", theme_arg);
        for (int i = 0; i < THEME_COUNT; i++) fprintf(stderr, "%s %d %s", i ? "," : "", i + 1, THEMES[i].name);
        fprintf(stderr, "\n");
        return 1;
    }

    if (!font_init()) { fprintf(stderr, "FLT: no font\n"); return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "FLT: SDL: %s\n", SDL_GetError()); return 1; }
    SDL_DisplayMode mode_buf, *mode = &mode_buf;
    if (!pick_mode(want_w, want_h, want_bpp, mode) && !pick_mode(0, 0, 0, mode)) { fprintf(stderr, "FLT: no VESA mode of 640x480 or more in 15 bits or more\n"); SDL_Quit(); return 1; }
    list_modes();
    s_mode = *mode;
    g_sdl_window = SDL_CreateWindow("Felix Terminal", mode->w, mode->h, SDL_WINDOW_FULLSCREEN);
    if (!g_sdl_window) { fprintf(stderr, "FLT: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetWindowFullscreenMode(g_sdl_window, mode);
    SDL_SyncWindow(g_sdl_window);
    g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, nullptr);
    if (!g_sdl_renderer) { SDL_Quit(); fprintf(stderr, "FLT: renderer: %s\n", SDL_GetError()); return 1; }
    new_text_layer();
    SDL_ShowCursor();                        // the mouse pointer (when there's a mouse)
    SDL_StartTextInput(g_sdl_window);

    apply_theme(theme < 0 ? 0 : theme);
    term_init(&s_term);                      // the cell, measured with s_ft_face
    s_cw = (int)s_term.cell_w;
    s_ch = (int)s_term.cell_h;
    s_term.cell_w = (float)s_cw;
    s_term.cell_h = (float)s_ch;
    term_resize(&s_term, s_w, s_h);
    uint32_t bg0 = rgb(tcolor_resolve(TCOLOR_PALETTE(0)));
    text_clear(bg0);
    kitty_init();
    sixel_init();

    char line[256];
    snprintf(line, sizeof line, "Felix Terminal for DOS  %dx%d, %d bits: %dx%d characters\r\n",
             s_w, s_h, SDL_BYTESPERPIXEL(mode->format) == 4 ? 32 : SDL_BITSPERPIXEL(mode->format), s_term.cols, s_term.rows);
    term_feed(&s_term, line, (int)strlen(line));

    // [user@]host[:port], asked for when not given
    if (target.empty()) target = prompt_line("Host ([user@]host[:port]): ", false);
    size_t at = target.rfind('@');
    if (at != std::string::npos) { cfg.user = target.substr(0, at); target = target.substr(at + 1); }
    size_t colon = target.rfind(':');
    if (colon != std::string::npos && target.find(':') == colon) {
        cfg.port = atoi(target.c_str() + colon + 1);
        target.resize(colon);
    }
    if (!target.empty() && target[0] == '[' && target.back() == ']') target = target.substr(1, target.size() - 2);
    cfg.host = target;
    if (!cfg.host.empty()) ssh_config_load(cfg.host.c_str(), cfg);   // ~/.ssh/config
    while (!s_quit && cfg.user.empty()) {
        snprintf(line, sizeof line, "login as (%s): ", cfg.host.c_str());
        cfg.user = prompt_line(line, false);
        if (cfg.user.empty()) break;
    }

    int rc = 1;
    if (!s_quit && !cfg.host.empty() && !cfg.user.empty()) {
        cfg.x11_forward = false;
        cfg.prompt_password = [](const char *p) { return prompt_line(p, true); };
        cfg.prompt_host_key = [](const char *p) { return prompt_line(p, false); };
        snprintf(line, sizeof line, "Connecting to %s@%s:%d ...\r\n", cfg.user.c_str(), cfg.host.c_str(), cfg.port);
        term_feed(&s_term, line, (int)strlen(line));
        render(&s_term);
        if (ssh_connect(cfg, &s_term)) {
            ssh_pty_resize(s_term.cols, s_term.rows);
            rc = 0;
            uint64_t last_render = 0, last_blink = SDL_GetTicks();
            bool need = true;
            while (!s_quit && ssh_active()) {
                bool got = ssh_read(&s_term);
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_EVENT_QUIT) s_quit = true;
                    else if (e.type == SDL_EVENT_KEY_DOWN && s_menu.open) {
                        if (e.key.key == SDLK_ESCAPE) s_menu.open = false;   // the menu: Esc closes it
                        need = true;
                    } else if (e.type == SDL_EVENT_KEY_DOWN) {
                        if (key_down(&s_term, e.key.key, e.key.mod)) need = true;
                    } else if (e.type == SDL_EVENT_TEXT_INPUT) {
                        if (s_menu.open) continue;
                        if (s_term.sb_offset) { s_term.sb_offset = 0; term_dirty_all(&s_term); need = true; }
                        send(e.text.text);
                    } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                               e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                        if (mouse_event(&s_term, e)) need = true;
                    }
                }
                uint64_t now = SDL_GetTicks();
                if (s_term.cursor_blink_enabled && now - last_blink >= 500) {
                    s_term.cursor_blink_phase = !s_term.cursor_blink_phase;
                    last_blink = now;
                    need = true;
                }
                if (got) need = true;
                // While output streams in, a frame every 40 ms; else at once.
                if (need && !term_sync_active(&s_term) && (!got || now - last_render >= 40)) {
                    render(&s_term);
                    last_render = now;
                    need = false;
                }
                if (!got && !need) SDL_Delay(5);
            }
            ssh_disconnect();
        } else {
            std::string err = ssh_last_user_error();
            if (err.empty()) err = "Connection failed.\r\n";
            term_feed(&s_term, err.c_str(), (int)err.size());
            term_feed(&s_term, "\r\nPress a key.", 14);
            render(&s_term);
            for (bool done = false; !done && !s_quit; ) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_QUIT) done = true;
                SDL_Delay(10);
            }
        }
    }
    SDL_Quit();
    if (rc == 0) printf("Connection to %s closed.\n", cfg.host.c_str());
    return rc;
}
