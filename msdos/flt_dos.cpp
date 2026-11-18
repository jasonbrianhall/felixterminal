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
//
//   FLT [-p port] [-l user] [-i keyfile] [-g WIDTHxHEIGHT] [-f size]
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

#include "../DejaVuMono.h"
#include "../DejaVuMonoBold.h"
#include "../DejaVuMonoOblique.h"
#include "../DejaVuMonoBoldOblique.h"
#include "../FreeMono.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>

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

static FT_Face load_face(const char *b64, size_t b64_size) {
    unsigned char *buf = nullptr;
    size_t sz = 0;
    FT_Face f = nullptr;
    if (base64_decode(b64, b64_size, &buf, &sz) == 0 && buf &&
        FT_New_Memory_Face(s_ft_lib, buf, (FT_Long)sz, 0, &f) == 0)
        return f;
    free(buf);
    return nullptr;
}

static bool font_init(void) {
    if (FT_Init_FreeType(&s_ft_lib)) return false;
    s_face[0] = load_face(DEJAVU_REGULAR_FONT_B64, DEJAVU_REGULAR_FONT_B64_SIZE);
    s_face[1] = load_face(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE);          // DejaVu Sans Mono Bold
    s_face[2] = load_face(DEJAVU_OBLIQUE_FONT_B64, DEJAVU_OBLIQUE_FONT_B64_SIZE);
    s_face[3] = load_face(DEJAVU_BOLDOBLIQUE_FONT_B64, DEJAVU_BOLDOBLIQUE_FONT_B64_SIZE);
    s_face_fallback = load_face(FREEMONO_FONT_B64, FREEMONO_FONT_B64_SIZE);
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
static SDL_Texture *s_text_tex;
static std::vector<uint32_t> s_text;   // the text layer, XRGB8888, s_w*s_h
static int          s_w, s_h;           // the screen
static int          s_ox = 2, s_oy = 2; // the grid's top-left (2 px, as felixterminal)
static int          s_cw, s_ch;         // the cell

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
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    for (int j = 0; j < h; j++) {
        uint32_t *p = &s_text[(size_t)(y + j) * s_w + x];
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
        if (sy < py || sy >= py + s_ch || sy >= s_h) continue;   // stays in its row
        uint32_t *row = &s_text[(size_t)sy * s_w];
        const uint8_t *a = &g.a[(size_t)y * g.w];
        for (int x = 0; x < g.w; x++) {
            int sx = x0 + x;
            if (a[x] && sx >= 0 && sx < s_w) row[sx] = blend(row[sx], fc, a[x]);
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

static void draw_row(Terminal *t, int row) {
    int py = s_oy + row * s_ch;
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

// The text layer's dirty rows into the texture, then the frame: text,
// images, cursor.
static void render(Terminal *t) {
    int lo = -1, hi = -1;
    for (int r = 0; r < t->rows; r++) {
        if (!term_row_is_dirty(t, r)) continue;
        draw_row(t, r);
        if (lo < 0) lo = r;
        hi = r;
    }
    term_clear_dirty(t);
    if (lo >= 0) {
        SDL_Rect band = { 0, s_oy + lo * s_ch, s_w, (hi - lo + 1) * s_ch };
        if (band.y + band.h > s_h) band.h = s_h - band.y;
        SDL_UpdateTexture(s_text_tex, &band, &s_text[(size_t)band.y * s_w], s_w * 4);
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
    SDL_RenderPresent(g_sdl_renderer);
}

// The VESA mode: -g / FLT_VIDEO (WIDTHxHEIGHT[xBITS]), else 1024x768, else
// the largest of at least 640x480. 32-bit first, then 16-bit.
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
    case SDLK_INSERT: send("\x1b[2~"); return true;
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
// PROMPTS (ssh_connect's password and host key questions; the host to use)
// ============================================================================

static bool s_quit;

static std::string prompt_line(const char *prompt, bool secret) {
    term_feed(&s_term, prompt, (int)strlen(prompt));
    std::string s;
    for (;;) {
        render(&s_term);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { s_quit = true; return ""; }
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
        SDL_Delay(10);
    }
}

// ============================================================================
// MAIN
// ============================================================================

static void usage(void) {
    printf("Felix Terminal for DOS: SSH with sixel and kitty graphics\n\n"
           "  FLT [options] [user@]host[:port] [command]\n\n"
           "  -p PORT        port (default 22)\n"
           "  -l USER        user name\n"
           "  -i KEYFILE     private key (default: ~/.ssh/id_ed25519, id_rsa, ...)\n"
           "  -g WxH[xBITS]  screen mode (default 1024x768; also SET FLT_VIDEO=)\n"
           "  -f SIZE        font size in pixels (default %d)\n\n"
           "~ is %%HOME%% (C:\\HOME if not set). Shift+PgUp/PgDn scroll back.\n"
           "TERM is xterm-256color: use timg -pk (kitty) or timg -ps (sixel).\n",
           FONT_SIZE_DEFAULT);
}

int main(int argc, char **argv) {
    SshConfig cfg;
    std::string target;
    int want_w = 0, want_h = 0, want_bpp = 0;
    const char *env = getenv("FLT_VIDEO");
    if (env) sscanf(env, "%dx%dx%d", &want_w, &want_h, &want_bpp);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-p") && i + 1 < argc) cfg.port = atoi(argv[++i]);
        else if (!strcmp(a, "-l") && i + 1 < argc) cfg.user = argv[++i];
        else if (!strcmp(a, "-i") && i + 1 < argc) cfg.key_path = argv[++i];
        else if (!strcmp(a, "-g") && i + 1 < argc) { want_bpp = 0; sscanf(argv[++i], "%dx%dx%d", &want_w, &want_h, &want_bpp); }
        else if (!strcmp(a, "-f") && i + 1 < argc) g_font_size = atoi(argv[++i]);
        else if (!strcmp(a, "-?") || !strcmp(a, "/?") || !strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "FLT: unknown option %s (FLT -? for help)\n", a); return 1; }
        else if (target.empty()) target = a;
        else { if (!cfg.command.empty()) cfg.command += ' '; cfg.command += a; }
    }
    if (g_font_size < FONT_SIZE_MIN) g_font_size = FONT_SIZE_MIN;
    if (g_font_size > FONT_SIZE_MAX) g_font_size = FONT_SIZE_MAX;
    if (!getenv("HOME")) setenv("HOME", "C:/HOME", 1);   // as vmdos's ssh

    if (!font_init()) { fprintf(stderr, "FLT: no font\n"); return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "FLT: SDL: %s\n", SDL_GetError()); return 1; }
    SDL_DisplayMode mode_buf, *mode = &mode_buf;
    if (!pick_mode(want_w, want_h, want_bpp, mode)) { fprintf(stderr, "FLT: no VESA mode of 640x480 or more in 15 bits or more\n"); SDL_Quit(); return 1; }
    g_sdl_window = SDL_CreateWindow("Felix Terminal", mode->w, mode->h, SDL_WINDOW_FULLSCREEN);
    if (!g_sdl_window) { fprintf(stderr, "FLT: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetWindowFullscreenMode(g_sdl_window, mode);
    SDL_SyncWindow(g_sdl_window);
    g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, nullptr);
    if (!g_sdl_renderer) { SDL_Quit(); fprintf(stderr, "FLT: renderer: %s\n", SDL_GetError()); return 1; }
    SDL_GetCurrentRenderOutputSize(g_sdl_renderer, &s_w, &s_h);
    s_text.assign((size_t)s_w * s_h, 0);
    s_text_tex = SDL_CreateTexture(g_sdl_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, s_w, s_h);
    SDL_HideCursor();
    SDL_StartTextInput(g_sdl_window);

    apply_theme(0);
    term_init(&s_term);                      // the cell, measured with s_ft_face
    s_cw = (int)s_term.cell_w;
    s_ch = (int)s_term.cell_h;
    s_term.cell_w = (float)s_cw;
    s_term.cell_h = (float)s_ch;
    term_resize(&s_term, s_w, s_h);
    uint32_t bg0 = rgb(tcolor_resolve(TCOLOR_PALETTE(0)));
    std::fill(s_text.begin(), s_text.end(), bg0);
    SDL_UpdateTexture(s_text_tex, nullptr, s_text.data(), s_w * 4);
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
                    else if (e.type == SDL_EVENT_KEY_DOWN) {
                        if (key_down(&s_term, e.key.key, e.key.mod)) need = true;
                    } else if (e.type == SDL_EVENT_TEXT_INPUT) {
                        if (s_term.sb_offset) { s_term.sb_offset = 0; term_dirty_all(&s_term); need = true; }
                        send(e.text.text);
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
