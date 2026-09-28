#include "terminal.h"
#include "term_pty.h"   // term_write, term_feed
#include "ft_font.h"    // s_ft_face, g_font_size
#include "gl_terminal.h" // TERM_COLS_DEFAULT etc.
#include "kitty_graphics.h"
#include "sixel_graphics.h"
#include "basic_graphics.h"
#include "term_width.h"
#include <string>
#include <vector>
#include <unordered_map>

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <sys/ioctl.h>
#endif

extern int g_font_size;
// g_sdl_window needed for OSC title; forward-declared here, defined in main
extern SDL_Window *g_sdl_window;
// Current window dimensions — needed to pass to basic_handle_osc for coordinate mapping.
// Defined in gl_terminal_main.cpp (local to main, but we only need a rough snapshot).
extern int g_basic_win_w;
extern int g_basic_win_h;

// When false, ESC _ (APC) sequences are silently discarded instead of being
// passed to kitty_handle_apc(). Set to false for SSH sessions where tmux
// and other multiplexers send APC sequences that aren't kitty graphics.
bool g_kitty_enabled = true;

// DEC Special Graphics charset — maps ASCII 0x5f..0x7e to the Unicode
// box-drawing / symbol glyphs used when G0 is designated as line-drawing
// (ESC(0), e.g. by ncurses' smacs/rmacs. Index 0 == 0x5f.
static const uint32_t DEC_LINE_DRAWING[] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, // _`abcdef
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, // ghijklmn
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, // opqrstuv
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, // wxyz{|}~
};

// ============================================================================
// SCROLLBACK
// ============================================================================

static void sb_push(Terminal *t, int row) {
    //SDL_Log("[Scroll] sb_push called: sb_buf=%p sb_cap=%d scroll_top=%d scroll_bot=%d rows=%d\n", (void*)t->sb_buf, t->sb_cap, t->scroll_top, t->scroll_bot, t->rows);
    if (!t->sb_buf || t->sb_cap == 0) return;
    if (t->scroll_top != 0 || t->scroll_bot != t->rows - 1) return;
    int slot = (t->sb_head + t->sb_count) % t->sb_cap;
    Cell *sb_row_ptr = t->sb_buf + slot * t->sb_cols;
    
    // Copy current row data (up to min of display width and scrollback width)
    int copy_cols = (t->cols < t->sb_cols) ? t->cols : t->sb_cols;
    memcpy(sb_row_ptr, &CELL(t, row, 0), sizeof(Cell) * copy_cols);
    
    // If scrollback is wider than display, pad the rest with blanks
    if (t->sb_cols > t->cols) {
        for (int c = t->cols; c < t->sb_cols; c++)
            sb_row_ptr[c] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    }
    
    if (t->sb_count < t->sb_cap) {
        t->sb_count++;
    } else {
        t->sb_head = (t->sb_head + 1) % t->sb_cap;
    }
    //if (t->sb_count <= 5 || t->sb_count % 50 == 0)
        //SDL_Log("[Scroll] sb_push: sb_count now %d\n", t->sb_count);
}

Cell* sb_row(Terminal *t, int idx) {
    int slot = (t->sb_head + idx) % t->sb_cap;
    return t->sb_buf + slot * t->sb_cols;
}

Cell* vcell(Terminal *t, int vrow, int col) {
    static Cell blank = {' ', 7, 0, 0, {0,0,0}};
    if (col < 0 || col >= t->cols) return &blank;
    if (vrow < 0) return &blank;
    if (vrow < t->sb_count) {
        // Scrollback row: clamp col to sb_cols (old data might be wider)
        int sb_col = (col < t->sb_cols) ? col : -1;
        if (sb_col < 0) return &blank;
        return sb_row(t, vrow) + sb_col;
    }
    int live = vrow - t->sb_count;
    if (live < t->rows) return &CELL(t, live, col);
    return &blank;
}

// ============================================================================
// SCROLL / CURSOR
// ============================================================================

static void scroll_up(Terminal *t) {
    int top = t->scroll_top;
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (top == 0 && !t->in_alt_screen) sb_push(t, top);
    if (bot > top)
        memmove(&CELL(t,top,0), &CELL(t,top+1,0), sizeof(Cell)*t->cols*(bot-top));
    for (int c = 0; c < t->cols; c++)
        CELL(t,bot,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
    term_dirty_rows(t, top, bot);
    // Shift image placements up with the scroll region
    if (top == 0 && bot == t->rows - 1) {
        kitty_scroll(t, 1);
        sixel_scroll(t, 1);
    }
}

static void scroll_down(Terminal *t) {
    int top = t->scroll_top;
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (bot > top)
        memmove(&CELL(t,top+1,0), &CELL(t,top,0), sizeof(Cell)*t->cols*(bot-top));
    for (int c = 0; c < t->cols; c++)
        CELL(t,top,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
    term_dirty_rows(t, top, bot);
}

static void newline(Terminal *t) {
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (t->cur_row < bot) { t->cur_row++; return; }
    if (t->cur_row == bot) { scroll_up(t); return; }
    if (t->cur_row < t->rows - 1) t->cur_row++;
}

// Public wrapper used by kitty_graphics to advance the cursor with proper scrolling
void term_newline(Terminal *t) { newline(t); }

// ============================================================================
// OSC 8 HYPERLINK TABLE — cells store a 16-bit id; ids map to URIs here.
// Identical (id-param, URI) pairs share one id so a link split across lines
// or redrawn by an app stays one link.
// ============================================================================

static std::vector<std::string>                  s_link_uris(1);  // [0] unused
static std::unordered_map<std::string, uint16_t> s_link_ids;

static uint16_t link_intern(const std::string &params, const std::string &uri) {
    std::string key = params + '\x01' + uri;
    auto it = s_link_ids.find(key);
    if (it != s_link_ids.end()) return it->second;
    if (s_link_uris.size() >= 65535) return 0;   // table full: plain text
    uint16_t id = (uint16_t)s_link_uris.size();
    s_link_uris.push_back(uri);
    s_link_ids.emplace(std::move(key), id);
    return id;
}

const char *term_link_uri(uint16_t id) {
    return (id && id < s_link_uris.size()) ? s_link_uris[id].c_str() : nullptr;
}

// If (row, col) is one half of a double-width character, blank the OTHER
// half so overwriting it never leaves an orphaned half-glyph behind.
static void break_wide_pair(Terminal *t, int row, int col) {
    if (col < 0 || col >= t->cols) return;
    Cell &c = CELL(t, row, col);
    if (cell_is_wide_tail(&c) && col > 0) {
        Cell &h = CELL(t, row, col - 1);
        h.cp = ' '; h._pad[0] &= ~(CELL_F_WIDE | CELL_F_WIDE_TAIL);
    }
    if (cell_is_wide(&c) && col + 1 < t->cols) {
        Cell &tl = CELL(t, row, col + 1);
        tl.cp = ' '; tl._pad[0] &= ~(CELL_F_WIDE | CELL_F_WIDE_TAIL);
    }
}

// Places one decoded codepoint at the cursor and advances it, handling
// autowrap. Shared by the ASCII path and the UTF-8 decode path below so
// both go through identical cell-write/advance/wrap behavior.
//
// Width-aware: combining marks (width 0) merge into the previous cell,
// wide characters (width 2) take two cells and wrap early if only one
// column is left.
static void term_put_char(Terminal *t, uint32_t cp) {
    int w = term_char_width(cp);

    if (w == 0) {
        // Zero-width (combining accent, ZWJ, variation selector...): never
        // advances. If it composes with the previous character (e + U+0301
        // -> é), store the precomposed form; otherwise drop it — a cell holds
        // a single codepoint.
        int row = t->cur_row, col = t->cur_col - 1;
        if (col < 0) {
            // Previous character autowrapped: it's at the end of the line above
            if (row == 0) return;
            row--; col = t->cols - 1;
        }
        if (col >= t->cols) col = t->cols - 1;
        Cell *prev = &CELL(t, row, col);
        if (cell_is_wide_tail(prev) && col > 0) prev = &CELL(t, row, col - 1);
        uint32_t composed = term_compose(prev->cp, cp);
        if (composed) {
            prev->cp = composed;
            term_dirty_row(t, row);
        }
        return;
    }

    if (w == 2) {
        if (t->cols < 2) w = 1;
        else if (t->cur_col >= t->cols - 1) {
            // Only one column left: pad it and wrap first, like xterm/VTE
            if (t->autowrap) {
                if (t->cur_col < t->cols) {
                    break_wide_pair(t, t->cur_row, t->cur_col);
                    CELL(t, t->cur_row, t->cur_col) = {' ', t->cur_fg, t->cur_bg, t->cur_attrs, {0,0,0}};
                    term_dirty_row(t, t->cur_row);
                }
                t->cur_col = 0;
                newline(t);
            } else {
                t->cur_col = t->cols - 2;   // no wrap: overwrite the last two cells
            }
        }
    }

    if (t->cur_col < t->cols) {
        int row = t->cur_row, col = t->cur_col;
        break_wide_pair(t, row, col);
        uint8_t ul = (t->cur_attrs & ATTR_UNDERLINE)
                   ? (uint8_t)((t->cur_ul_style << CELL_UL_SHIFT) & CELL_UL_MASK) : 0;
        if (t->cur_hidden) ul |= CELL_F_HIDDEN;
        uint8_t lk0 = (uint8_t)(t->cur_link & 0xFF), lk1 = (uint8_t)(t->cur_link >> 8);
        uint32_t ulc = (t->cur_attrs & ATTR_UNDERLINE) ? t->cur_ul_color : 0;
        if (w == 2) {
            break_wide_pair(t, row, col + 1);
            CELL(t, row, col)     = {cp, t->cur_fg, t->cur_bg, t->cur_attrs, {(uint8_t)(CELL_F_WIDE | ul), lk0, lk1}, ulc};
            CELL(t, row, col + 1) = {0,  t->cur_fg, t->cur_bg, t->cur_attrs, {(uint8_t)(CELL_F_WIDE_TAIL | ul), lk0, lk1}, ulc};
        } else {
            CELL(t, row, col)     = {cp, t->cur_fg, t->cur_bg, t->cur_attrs, {ul, lk0, lk1}, ulc};
        }
        term_dirty_row(t, row);
    }
    t->last_cp = cp;
    t->cur_col += w;
    if (t->cur_col >= t->cols) {
        if (t->autowrap) {
            t->cur_col = 0;
            newline(t);
        } else {
            // ?7l: no wrap — stay on the last column; the next character
            // overwrites it (xterm/VT100 behaviour)
            t->cur_col = t->cols - 1;
        }
    }
}

// ============================================================================
// SGR
// ============================================================================

// Parse an extended color starting at group[gi] (value 38/48/58).
// Handles both forms:
//   semicolon:  38;5;N   38;2;R;G;B            (consumes following groups)
//   colon:      38:5:N   38:2::R:G:B  38:2:R:G:B   38:2:CS:R:G:B (one group)
// Returns true and sets *out if a color was parsed; *gi is advanced past any
// groups consumed.
struct SgrGroup { int v[8]; int n; };

static bool sgr_ext_color(const SgrGroup *g, int ng, int *gi, TermColorVal *out) {
    const SgrGroup &cur = g[*gi];
    auto c8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    if (cur.n > 1) {                       // colon form, self-contained
        int mode = cur.v[1];
        if (mode == 5 && cur.n >= 3) { *out = TCOLOR_PALETTE(c8(cur.v[2])); return true; }
        if (mode == 2) {
            // 38:2:CS:R:G:B (6+ subs, CS = colorspace, usually empty) or 38:2:R:G:B
            int o = (cur.n >= 6) ? 3 : 2;
            if (cur.n >= o + 3) {
                *out = TCOLOR_RGB(c8(cur.v[o]), c8(cur.v[o+1]), c8(cur.v[o+2]));
                return true;
            }
        }
        return false;
    }
    // semicolon form
    int i = *gi;
    if (i + 2 < ng && g[i+1].v[0] == 5) {
        *out = TCOLOR_PALETTE(c8(g[i+2].v[0])); *gi = i + 2; return true;
    }
    if (i + 4 < ng && g[i+1].v[0] == 2) {
        *out = TCOLOR_RGB(c8(g[i+2].v[0]), c8(g[i+3].v[0]), c8(g[i+4].v[0]));
        *gi = i + 4; return true;
    }
    return false;
}

static void sgr(Terminal *t, const char *p) {
    // Split into ';'-separated groups, each with ':'-separated sub-params.
    // Empty fields count as 0 (ECMA-48 default).
    SgrGroup g[32];
    int ng = 0;
    {
        SgrGroup cur = {{0}, 1};
        bool any = false;
        for (const char *q = p; ; q++) {
            char ch = *q;
            if (ch >= '0' && ch <= '9') {
                int &v = cur.v[cur.n - 1];
                if (v < 100000) v = v * 10 + (ch - '0');
                any = true;
            } else if (ch == ':') {
                if (cur.n < 8) cur.v[cur.n++] = 0;
                any = true;
            } else if (ch == ';' || ch == '\0') {
                if (ng < 32) g[ng++] = cur;
                cur = SgrGroup{{0}, 1};
                if (ch == '\0') break;
                any = true;
            }
        }
        if (!any) ng = 1;   // "CSI m" == "CSI 0 m" (g[0] is already {0})
    }

    for (int i = 0; i < ng; i++) {
        int v = g[i].v[0];
        if (v == 0) {
            t->cur_fg = TCOLOR_PALETTE(7); t->cur_bg = TCOLOR_PALETTE(0); t->cur_attrs = 0;
            t->cur_ul_style = UL_SINGLE; t->cur_ul_color = 0;
            t->cur_hidden = false;
        }
        else if (v == 1)  t->cur_attrs |= ATTR_BOLD;
        else if (v == 2)  t->cur_attrs |= ATTR_DIM;
        else if (v == 3)  t->cur_attrs |= ATTR_ITALIC;
        else if (v == 4) {
            // 4 = single; 4:0 none, 4:1 single, 4:2 double, 4:3 curly,
            // 4:4 dotted, 4:5 dashed
            int style = (g[i].n > 1) ? g[i].v[1] : 1;
            if (style == 0) t->cur_attrs &= ~ATTR_UNDERLINE;
            else {
                t->cur_attrs |= ATTR_UNDERLINE;
                t->cur_ul_style = (style >= 2 && style <= 5) ? (uint8_t)(style - 1) : UL_SINGLE;
            }
        }
        else if (v == 5)  t->cur_attrs |= ATTR_BLINK;
        else if (v == 7)  t->cur_attrs |= ATTR_REVERSE;
        else if (v == 8)  t->cur_hidden = true;
        else if (v == 9)  t->cur_attrs |= ATTR_STRIKE;
        else if (v == 21) { t->cur_attrs |= ATTR_UNDERLINE; t->cur_ul_style = UL_DOUBLE; }
        else if (v == 22) t->cur_attrs &= ~(ATTR_BOLD | ATTR_DIM);
        else if (v == 23) t->cur_attrs &= ~ATTR_ITALIC;
        else if (v == 24) { t->cur_attrs &= ~ATTR_UNDERLINE; t->cur_ul_style = UL_SINGLE; }
        else if (v == 25) t->cur_attrs &= ~ATTR_BLINK;
        else if (v == 27) t->cur_attrs &= ~ATTR_REVERSE;
        else if (v == 28) t->cur_hidden = false;
        else if (v == 29) t->cur_attrs &= ~ATTR_STRIKE;
        else if (v == 53) t->cur_attrs |= ATTR_OVERLINE;
        else if (v == 55) t->cur_attrs &= ~ATTR_OVERLINE;
        else if (v >= 30 && v <= 37)   t->cur_fg = TCOLOR_PALETTE(v - 30);
        else if (v == 38) { TermColorVal c; if (sgr_ext_color(g, ng, &i, &c)) t->cur_fg = c; }
        else if (v == 39)              t->cur_fg = TCOLOR_PALETTE(7);
        else if (v >= 40 && v <= 47)   t->cur_bg = TCOLOR_PALETTE(v - 40);
        else if (v == 48) { TermColorVal c; if (sgr_ext_color(g, ng, &i, &c)) t->cur_bg = c; }
        else if (v == 49)              t->cur_bg = TCOLOR_PALETTE(0);
        else if (v == 58) { TermColorVal c; if (sgr_ext_color(g, ng, &i, &c)) t->cur_ul_color = CELL_UL_COLOR_SET | c; }
        else if (v == 59)              t->cur_ul_color = 0;
        else if (v >= 90 && v <= 97)   t->cur_fg = TCOLOR_PALETTE(v - 90 + 8);
        else if (v >= 100 && v <= 107) t->cur_bg = TCOLOR_PALETTE(v - 100 + 8);
    }
}

// ============================================================================
// CSI DISPATCH
// ============================================================================

static void dispatch_csi(Terminal *t) {
    if (!t->csi_len) return;
    char final = t->csi[t->csi_len-1];
    t->csi[t->csi_len-1] = '\0';
    const char *p = t->csi;
    // Sequences with intermediate bytes (e.g. CSI 2 SP q = cursor style,
    // CSI SP @ = scroll left) are different commands that share a final byte
    // with ones handled below — none are supported yet, so ignore them
    // rather than misreading them.
    char inter = 0;
    for (const char *q = p; *q; q++)
        if (*q >= 0x20 && *q <= 0x2F) { inter = *q; break; }
    if (inter) {
        if (inter == ' ' && final == 'q') {
            // DECSCUSR — cursor style: 0/1 blinking block, 2 steady block,
            // 3/4 blinking/steady underline, 5/6 blinking/steady bar.
            // 0 restores the user's own style (captured on first change).
            if (!t->cursor_default_saved) {
                t->cursor_default_saved = true;
                t->cursor_default_shape = t->cursor_shape;
                t->cursor_default_blink = t->cursor_blink_enabled;
            }
            int n = atoi(p);
            if (n == 0) {
                t->cursor_shape         = t->cursor_default_shape;
                t->cursor_blink_enabled = t->cursor_default_blink;
            } else if (n <= 6) {
                static const int shape[7] = { 0, 0, 0, 1, 1, 2, 2 };
                t->cursor_shape         = shape[n];
                t->cursor_blink_enabled = (n % 2) == 1;
            }
            // Restart the blink cycle lit, so the new shape shows immediately
            t->cursor_blink_phase = true;
            t->cursor_blink       = 0;
            term_dirty_row(t, t->cur_row);
        }
        else if (inter == '$' && final == 'p' && p[0] == '?') {
            // DECRQM — "is private mode N set?" Apps use it to detect
            // features (notably ?2026 synchronized output) before using them.
            // Reply: CSI ? N ; S $ y  with S = 1 set, 2 reset, 0 unknown.
            int mode = atoi(p + 1), st = 0;
            switch (mode) {
            case 1:    st = t->app_cursor_keys ? 1 : 2; break;
            case 7:    st = t->autowrap        ? 1 : 2; break;
            case 25:   st = t->cursor_on       ? 1 : 2; break;
            case 1000: case 1002: case 1003:
                       st = t->mouse_report    ? 1 : 2; break;
            case 1006: st = t->mouse_sgr       ? 1 : 2; break;
            case 47: case 1047: case 1049:
                       st = t->in_alt_screen   ? 1 : 2; break;
            case 2004: st = t->bracketed_paste ? 1 : 2; break;
            case 2026: st = t->sync_output     ? 1 : 2; break;
            }
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "\x1b[?%d;%d$y", mode, st);
            term_write(t, resp, len);
        }
        // Other intermediate sequences (CSI SP @ scroll-left, CSI ! p soft
        // reset, CSI $ ...) share final bytes with commands below — ignore
        // them rather than misreading them.
        t->csi_len = 0;
        return;
    }
    // Private-prefixed 'm' (e.g. vim's "CSI > 4;2 m" modifyOtherKeys) is NOT
    // SGR — treating it as one reset all text attributes.
    if (final == 'm' && (p[0] == '<' || p[0] == '=' || p[0] == '>' || p[0] == '?')) {
        t->csi_len = 0;
        return;
    }
    switch (final) {
    case 'm': sgr(t, p); break;
    case 'H': case 'f': {
        int row=1,col=1; sscanf(p,"%d;%d",&row,&col);
        t->cur_row = SDL_clamp(row-1, 0, t->rows-1);
        t->cur_col = SDL_clamp(col-1, 0, t->cols-1);
        break;
    }
    case 'A': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_max(0,t->cur_row-n); break; }
    case 'B': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_min(t->rows-1,t->cur_row+n); break; }
    case 'C': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_min(t->cols-1,t->cur_col+n); break; }
    case 'D': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_max(0,t->cur_col-n); break; }
    case 'G': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_clamp(n-1,0,t->cols-1); break; }
    case 'd': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_clamp(n-1,0,t->rows-1); break; }
    case 'e': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_min(t->rows-1,t->cur_row+n); break; }
    // CNL / CPL — cursor next/previous line, to column 1
    case 'E': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_min(t->rows-1,t->cur_row+n); t->cur_col=0; break; }
    case 'F': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_max(0,t->cur_row-n);         t->cur_col=0; break; }
    // HPA / HPR — absolute / relative column
    case '`': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_clamp(n-1,0,t->cols-1); break; }
    case 'a': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_min(t->cols-1,t->cur_col+n); break; }
    // SU / SD — scroll the region up / down n lines, cursor stays put
    case 'S': {
        if (p[0] == '?') {
            // XTSMGRAPHICS query (CSI ? Pi ; Pa ; Pv S) — sixel tools ask for
            // color registers (Pi=1) and max geometry (Pi=2). Pa=1 = read.
            // Reply CSI ? Pi ; Ps ; Pv S (Ps 0 = ok, 1 = unknown item).
            int pi = 0, pa = 0;
            sscanf(p + 1, "%d;%d", &pi, &pa);
            char resp[48]; int len = 0;
            if      (pi == 1 && (pa == 1 || pa == 4)) len = snprintf(resp, sizeof(resp), "\x1b[?1;0;256S");
            else if (pi == 2 && (pa == 1 || pa == 4)) len = snprintf(resp, sizeof(resp), "\x1b[?2;0;4096;4096S");
            else                                      len = snprintf(resp, sizeof(resp), "\x1b[?%d;1;0S", pi);
            term_write(t, resp, len);
            break;
        }
        int n=atoi(p); if(n<1)n=1;
        n = SDL_min(n, t->rows);
        for (int i = 0; i < n; i++) scroll_up(t);
        break;
    }
    case 'T': {
        // CSI Ps;Ps;Ps;Ps;Ps T = mouse highlight tracking and CSI > Ps T =
        // title-mode reset — neither is SD
        if (strchr(p, ';') || (p[0] && (p[0] < '0' || p[0] > '9'))) break;
        int n=atoi(p); if(n<1)n=1;
        n = SDL_min(n, t->rows);
        for (int i = 0; i < n; i++) scroll_down(t);
        break;
    }
    // SCOSC / SCORC — save / restore cursor (same slot as ESC 7 / ESC 8)
    case 's':
        if (p[0] == '?') break;       // CSI ? Pm s = XTSAVE (save DEC modes), not supported
        t->saved7_row = t->cur_row;   t->saved7_col = t->cur_col;
        t->saved7_fg  = t->cur_fg;    t->saved7_bg  = t->cur_bg;
        t->saved7_attrs = t->cur_attrs;
        break;
    case 'u':
        if (p[0]) break;              // CSI > u / CSI = u ... are kitty keyboard protocol
        t->cur_row = SDL_clamp(t->saved7_row, 0, t->rows - 1);
        t->cur_col = SDL_clamp(t->saved7_col, 0, t->cols - 1);
        t->cur_fg  = t->saved7_fg;    t->cur_bg  = t->saved7_bg;
        t->cur_attrs = t->saved7_attrs;
        break;
    // REP — repeat the last printed character n times (ncurses uses it
    // for long runs of the same character)
    case 'b': {
        int n=atoi(p); if(n<1)n=1;
        if (!t->last_cp) break;
        n = SDL_min(n, t->cols * t->rows);
        uint32_t cp = t->last_cp;
        for (int i = 0; i < n; i++) term_put_char(t, cp);
        break;
    }
    case 'J': {
        int n=atoi(p);
        if (n==3) {
            // ED 3 — erase the scrollback only (xterm). `clear` sends this
            // after ED 2 so old output can't be scrolled back to.
            t->sb_count  = 0;
            t->sb_head   = 0;
            t->sb_offset = 0;
            t->sel_exists = t->sel_active = false;
            kitty_scroll(t, 0);   // lines=0: just drops images that were
            sixel_scroll(t, 0);   // only reachable in the scrollback
            term_dirty_all(t);
        } else if (n==2) {
            for(int r=0;r<t->rows;r++) for(int c=0;c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            t->cur_row=t->cur_col=0;
            kitty_erase_screen(t);   // images on screen go with their text
            sixel_erase_screen(t);
            term_dirty_all(t);
        } else if(n==1) {
            // Start of screen through the cursor, inclusive
            for(int r=0;r<t->cur_row;r++) for(int c=0;c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            for(int c=0;c<=t->cur_col && c<t->cols;c++) CELL(t,t->cur_row,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            term_dirty_rows(t, 0, t->cur_row);
        } else {
            for(int r=t->cur_row;r<t->rows;r++)
                for(int c=(r==t->cur_row?t->cur_col:0);c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            term_dirty_rows(t, t->cur_row, t->rows - 1);
        }
        break;
    }
    case 'K': {
        int n=atoi(p);
        int s=(n==1)?0:t->cur_col, e=(n==0)?t->cols:t->cur_col+1;
        for(int c=s;c<e&&c<t->cols;c++) CELL(t,t->cur_row,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        term_dirty_row(t, t->cur_row);
        break;
    }
    case 'h': case 'l': {
        // DECSET / DECRST. Private modes are prefixed with '?' in the CSI
        // buffer (e.g. "?1049") — atoi() on that returns 0, so it must be
        // skipped before parsing, or every private mode silently no-ops.
        bool priv = (p[0] == '?');
        int mode = atoi(priv ? p + 1 : p);
        bool set = (final == 'h');
        if (priv) {
            switch (mode) {
            case 25: t->cursor_on = set; break;
            case 1:  t->app_cursor_keys = set; break;
            case 1000: case 1002: case 1003: t->mouse_report = set; break;
            case 1006: t->mouse_sgr = set; break;
            case 2004: t->bracketed_paste = set; break;
            case 7:    t->autowrap = set; break;
            case 2026:
                t->sync_output = set;
                if (set) t->sync_start_ms = SDL_GetTicks();
                break;
            case 47: case 1047: case 1049: {
                // Alternate screen buffer (smcup/rmcup) — used by ncurses
                // apps like top/sl/vim/less. Without this their full-screen
                // redraws land directly on the live/main buffer instead of
                // a separate one, mixing with whatever was already there.
                if (set && !t->in_alt_screen) {
                    if (mode == 1049) {
                        t->saved_cur_row   = t->cur_row;
                        t->saved_cur_col   = t->cur_col;
                        t->saved_cur_fg    = t->cur_fg;
                        t->saved_cur_bg    = t->cur_bg;
                        t->saved_cur_attrs = t->cur_attrs;
                    }
                    if (!t->alt_cells)
                        t->alt_cells = (Cell*)malloc(sizeof(Cell) * t->cols * t->rows);
                    Cell *tmp = t->cells;
                    t->cells = t->alt_cells;
                    t->alt_cells = tmp;
                    for (int i = 0; i < t->cols * t->rows; i++)
                        t->cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
                    t->in_alt_screen = true;
                    t->cur_row = t->cur_col = 0;
                    t->scroll_top = 0; t->scroll_bot = t->rows - 1;
                    term_dirty_all(t);
                } else if (!set && t->in_alt_screen) {
                    Cell *tmp = t->cells;
                    t->cells = t->alt_cells;
                    t->alt_cells = tmp;
                    t->in_alt_screen = false;
                    // The alternate screen is discarded — so are its images
                    kitty_leave_alt_screen(t);
                    sixel_leave_alt_screen(t);
                    t->scroll_top = 0; t->scroll_bot = t->rows - 1;
                    if (mode == 1049) {
                        t->cur_row   = SDL_clamp(t->saved_cur_row, 0, t->rows - 1);
                        t->cur_col   = SDL_clamp(t->saved_cur_col, 0, t->cols - 1);
                        t->cur_fg    = t->saved_cur_fg;
                        t->cur_bg    = t->saved_cur_bg;
                        t->cur_attrs = t->saved_cur_attrs;
                    }
                    term_dirty_all(t);
                }
                break;
            }
            }
        }
        break;
    }
    case 'M': {
        // DL - delete line(s): remove n lines at cursor row, pulling rows
        // below up and blanking the bottom of the scroll region.
        int n = atoi(p); if (n < 1) n = 1;
        int bot = SDL_min(t->scroll_bot, t->rows - 1);
        if (t->cur_row >= t->scroll_top && t->cur_row <= bot) {
            int save_top = t->scroll_top;
            t->scroll_top = t->cur_row;
            for (int i = 0; i < n; i++) scroll_up(t);
            t->scroll_top = save_top;
        }
        break;
    }
    case 'L': {
        // IL - insert line(s): push rows at/after cursor down, blanking
        // n lines at the cursor row within the scroll region.
        int n = atoi(p); if (n < 1) n = 1;
        int bot = SDL_min(t->scroll_bot, t->rows - 1);
        if (t->cur_row >= t->scroll_top && t->cur_row <= bot) {
            int save_top = t->scroll_top;
            t->scroll_top = t->cur_row;
            for (int i = 0; i < n; i++) scroll_down(t);
            t->scroll_top = save_top;
        }
        break;
    }
    case 'P': {
        // DCH - delete character(s): shift cells after cursor left,
        // blank the vacated cells at end of line.
        int n = atoi(p); if (n < 1) n = 1;
        int row = t->cur_row, col = t->cur_col;
        if (col < t->cols) {
            int count = t->cols - col;
            if (n > count) n = count;
            int remain = count - n;
            if (remain > 0)
                memmove(&CELL(t,row,col), &CELL(t,row,col+n), sizeof(Cell)*remain);
            for (int c = t->cols - n; c < t->cols; c++)
                CELL(t,row,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
            term_dirty_row(t, row);
        }
        break;
    }
    case '@': {
        // ICH - insert character(s): shift cells at/after cursor right,
        // blank n cells at the cursor.
        int n = atoi(p); if (n < 1) n = 1;
        int row = t->cur_row, col = t->cur_col;
        if (col < t->cols) {
            int count = t->cols - col;
            if (n > count) n = count;
            int remain = count - n;
            if (remain > 0)
                memmove(&CELL(t,row,col+n), &CELL(t,row,col), sizeof(Cell)*remain);
            for (int c = col; c < col + n && c < t->cols; c++)
                CELL(t,row,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
            term_dirty_row(t, row);
        }
        break;
    }
    case 'X': {
        // ECH - erase character(s): blank n cells from cursor, no shifting.
        int n = atoi(p); if (n < 1) n = 1;
        int row = t->cur_row;
        for (int c = t->cur_col; c < t->cur_col + n && c < t->cols; c++)
            CELL(t,row,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
        term_dirty_row(t, row);
        break;
    }
    case 'r': {
        // DECSTBM - set scroll region (1-based, inclusive). No args = full screen.
        int top = 1, bot = t->rows;
        if (p[0]) sscanf(p, "%d;%d", &top, &bot);
        top = SDL_clamp(top, 1, t->rows) - 1;
        bot = SDL_clamp(bot, 1, t->rows) - 1;
        if (top < bot) {
            t->scroll_top = top;
            t->scroll_bot = bot;
        } else {
            t->scroll_top = 0;
            t->scroll_bot = t->rows - 1;
        }
        t->cur_row = t->scroll_top;
        t->cur_col = 0;
        break;
    }
    case 'q':
        // XTVERSION (CSI > q / CSI > 0 q) — report terminal name and version
        // as DCS > | name(version) ST. (CSI Ps SP q, cursor style, has an
        // intermediate byte and is handled above.)
        if (p[0] == '>') {
            char resp[64];
            int len = snprintf(resp, sizeof(resp), "\x1bP>|%s(%s)\x1b\\",
                               FELIX_TERM_NAME, FELIX_TERM_VERSION);
            term_write(t, resp, len);
        }
        break;
    case 'c': {
        // DA - device attributes. '>' prefix = secondary (DA2), else primary (DA1).
        char resp[32];
        int len;
        if (p[0] == '>')
            len = snprintf(resp, sizeof(resp), "\x1b[>1;100;0c");
        else
            len = snprintf(resp, sizeof(resp), "\x1b[?1;2;4c");  // 4 = sixel graphics
        term_write(t, resp, len);
        break;
    }
    case 'n': {
        // DSR - device status report
        int n = atoi(p);
        char resp[32];
        if (n == 6) {
            int len = snprintf(resp, sizeof(resp), "\x1b[%d;%dR", t->cur_row+1, t->cur_col+1);
            term_write(t, resp, len);
        } else if (n == 5) {
            const char *ok = "\x1b[0n";
            term_write(t, ok, (int)strlen(ok));
        }
        break;
    }
    case 't': {
        // Window manipulation - only report queries are answered; the rest
        // (raise/lower/iconify/move/resize) are no-ops since we're windowed.
        int op = atoi(p);
        char resp[64];
        int len = 0;
        if (op == 18) {
            // Report text area size in characters
            len = snprintf(resp, sizeof(resp), "\x1b[8;%d;%dt", t->rows, t->cols);
        } else if (op == 14) {
            // Report text area size in pixels
            int px_w = (int)(t->cols * t->cell_w);
            int px_h = (int)(t->rows * t->cell_h);
            len = snprintf(resp, sizeof(resp), "\x1b[4;%d;%dt", px_h, px_w);
        } else if (op == 16) {
            // Report character cell size in pixels. timg, chafa and others
            // need this to use kitty/sixel graphics when the pty doesn't
            // carry pixel sizes (built-in SSH, telnet, serial).
            len = snprintf(resp, sizeof(resp), "\x1b[6;%d;%dt",
                           (int)t->cell_h, (int)t->cell_w);
        }
        if (len > 0) term_write(t, resp, len);
        break;
    }
    }
    t->csi_len = 0;
}

// ============================================================================
// OSC DISPATCH
// ============================================================================

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// `s` is NUL-terminated, `len` bytes. `bel` = terminated by BEL (reply the same way).
static void dispatch_osc(Terminal *t, char *s, int len, bool bel) {
    char *semi = strchr(s, ';');
    if (!semi) return;
    int ps = atoi(s);
    char *arg = semi + 1;

    if ((ps == 0 || ps == 2) && g_sdl_window) {
        SDL_SetWindowTitle(g_sdl_window, arg);
    }
    else if ((ps == 10 || ps == 11) && strcmp(arg, "?") == 0) {
        // OSC 10/11 query: report default foreground/background color.
        // timg and others use this to blend transparent images onto the
        // real background (sixel has no alpha).
        float r, g, b;
        if (ps == 11) {
            const Theme &th = THEMES[g_theme_idx];
            r = th.bg_r; g = th.bg_g; b = th.bg_b;
        } else {
            TermColor fg = tcolor_resolve(TCOLOR_PALETTE(7));
            r = fg.r; g = fg.g; b = fg.b;
        }
        auto c16 = [](float v) { return (int)(SDL_clamp(v, 0.f, 1.f) * 65535.f + .5f); };
        char resp[64];
        int n = snprintf(resp, sizeof(resp), "\x1b]%d;rgb:%04x/%04x/%04x%s",
                         ps, c16(r), c16(g), c16(b), bel ? "\x07" : "\x1b\\");
        term_write(t, resp, n);
    }
    else if (ps == 8) {
        // OSC 8 ; params ; URI — start a hyperlink; empty URI ends it.
        // params is "id=xyz:key=val" (only id matters: it groups cells).
        char *semi2 = strchr(arg, ';');
        if (!semi2) return;
        std::string params(arg, semi2 - arg);
        std::string uri(semi2 + 1);
        if (uri.empty()) { t->cur_link = 0; return; }
        std::string id;
        size_t ip = params.find("id=");
        if (ip != std::string::npos) {
            size_t e = params.find(':', ip);
            id = params.substr(ip + 3, e == std::string::npos ? std::string::npos : e - ip - 3);
        }
        t->cur_link = link_intern(id, uri);
    }
    else if (ps == 52) {
        // OSC 52 ; selection ; base64 — set the clipboard (neovim/tmux/ssh
        // "yank to system clipboard"). Reading the clipboard ("?") is refused:
        // any program — including one on a remote host — could otherwise
        // silently read whatever you last copied.
        char *semi2 = strchr(arg, ';');
        if (!semi2) return;
        const char *data = semi2 + 1;
        if (strcmp(data, "?") == 0) return;
        std::string out;
        out.reserve((len * 3) / 4);
        uint32_t acc = 0; int bits = 0;
        for (const char *q = data; *q; q++) {
            int v = b64_val((unsigned char)*q);
            if (v < 0) continue;              // skip '=', whitespace
            acc = (acc << 6) | (uint32_t)v; bits += 6;
            if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
        }
        SDL_SetClipboardText(out.c_str());    // empty payload clears it
        SDL_Log("[OSC52] clipboard set by application (%zu bytes)\n", out.size());
    }
    else if (ps == 666) {
        basic_handle_osc(t, arg, (int)(s + len - arg), g_basic_win_w, g_basic_win_h);
    }
}

void term_feed(Terminal *t, const char *data, int size) {
    for (int i = 0; i < size; i++) {
        unsigned char ch = (unsigned char)data[i];
        int prev_row = t->cur_row, prev_col = t->cur_col;

        switch (t->state) {
        case PS_NORMAL:
            if (ch == 0x1b) {
                t->utf8_remaining = 0;  // abort any partial UTF-8 sequence
                t->state = PS_ESC;
            } else if (ch == 0x08) {
                if (t->cur_col > 0) t->cur_col--;
            } else if (ch == 0x09) {
                t->cur_col = (t->cur_col + 8) & ~7;
                if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
            } else if (ch == 0x0a || ch == 0x0b || ch == 0x0c) {
                newline(t);
            } else if (ch == 0x0d) {
                t->cur_col = 0;
            } else if (ch >= 32 && ch < 127) {
                uint32_t cp = ch;
                if (t->g0_line_drawing && ch >= 0x5f && ch <= 0x7e)
                    cp = DEC_LINE_DRAWING[ch - 0x5f];
                term_put_char(t, cp);
            } else if (ch >= 0xC2 && ch <= 0xF4) {
                // UTF-8 lead byte — start (or restart) accumulating a
                // multi-byte codepoint. Ranges: 110xxxxx (2-byte),
                // 1110xxxx (3-byte), 11110xxx (4-byte).
                if      (ch < 0xE0) { t->utf8_cp = ch & 0x1F; t->utf8_remaining = 1; }
                else if (ch < 0xF0) { t->utf8_cp = ch & 0x0F; t->utf8_remaining = 2; }
                else                 { t->utf8_cp = ch & 0x07; t->utf8_remaining = 3; }
            } else if (ch >= 0x80 && ch <= 0xBF) {
                if (t->utf8_remaining > 0) {
                    t->utf8_cp = (t->utf8_cp << 6) | (ch & 0x3F);
                    t->utf8_remaining--;
                    if (t->utf8_remaining == 0)
                        term_put_char(t, t->utf8_cp);
                }
                // else: stray continuation byte with no lead byte — drop it
            } else if (ch == 0x7f) {
                // DEL is treated as backspace
                if (t->cur_col > 0) {
                    t->cur_col--;
                    CELL(t, t->cur_row, t->cur_col) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
                    term_dirty_row(t, t->cur_row);
                }
            }
            break;

        case PS_ESC:
            if (ch == '[') {
                t->csi_len = 0;
                t->state = PS_CSI;
            } else if (ch == ']') {
                t->osc_len = 0;
                t->state = PS_OSC;
            } else if (ch == '_') {
                t->apc_len = 0;
                t->apc_esc_pending = false;
                t->state = PS_APC;
            } else if (ch == 'P') {
                t->dcs_len = 0;
                t->dcs_params_len = 0;
                t->dcs_is_sixel = false;
                t->dcs_determined = false;
                t->apc_esc_pending = false;
                t->state = PS_DCS;
            } else if (ch == '^') {
                t->state = PS_PM;
            } else if (ch == 'W') {
                t->state = PS_SOS;
            } else if (ch == 'c') {
                // RIS — reset terminal (do NOT call term_init here: that
                // wipes pty_fd/child and kills the live shell connection)
                term_soft_reset(t);
                t->state = PS_NORMAL;
            } else if (ch == 'M') {
                // RI — reverse index (move up)
                int top = t->scroll_top;
                if (t->cur_row > top) {
                    t->cur_row--;
                } else {
                    scroll_down(t);
                }
                t->state = PS_NORMAL;
            } else if (ch == 'E') {
                // NEL — move to next line
                t->cur_col = 0;
                newline(t);
                t->state = PS_NORMAL;
            } else if (ch == '7') {
                // DECSC — save cursor & attributes
                t->saved7_row = t->cur_row;
                t->saved7_col = t->cur_col;
                t->saved7_fg = t->cur_fg;
                t->saved7_bg = t->cur_bg;
                t->saved7_attrs = t->cur_attrs;
                t->state = PS_NORMAL;
            } else if (ch == '8') {
                // DECRC — restore cursor & attributes
                t->cur_row = SDL_clamp(t->saved7_row, 0, t->rows - 1);
                t->cur_col = SDL_clamp(t->saved7_col, 0, t->cols - 1);
                t->cur_fg = t->saved7_fg;
                t->cur_bg = t->saved7_bg;
                t->cur_attrs = t->saved7_attrs;
                t->state = PS_NORMAL;
            } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+') {
                // Gn charset designation (SCS) — e.g. ESC(B (ASCII),
                // ESC(0 (DEC line drawing). The next byte is the charset
                // designator and must be swallowed here, not left to fall
                // through to the else-branch below, where it would print
                // as literal text (this is the 'B' bug from `top`).
                t->charset_slot = ch;
                t->state = PS_CHARSET;
            } else {
                t->state = PS_NORMAL;
            }
            break;

        case PS_CHARSET:
            if (t->charset_slot == '(')
                t->g0_line_drawing = (ch == '0');
            t->state = PS_NORMAL;
            break;

        case PS_CSI:
            // ECMA-48: parameter bytes 0x30-0x3F (digits ; : < = > ?),
            // intermediate bytes 0x20-0x2F (space ! " $ ' ...), final byte
            // 0x40-0x7E. The final byte used to be limited to A-Z/a-z, which
            // silently dropped ICH (CSI @) — readline's insert-character — so
            // typing after Home overwrote text instead of inserting.
            if (ch >= 0x20 && ch <= 0x3F) {
                if (t->csi_len < (int)sizeof(t->csi) - 1) t->csi[t->csi_len++] = (char)ch;
            } else if (ch >= 0x40 && ch <= 0x7E) {
                if (t->csi_len < (int)sizeof(t->csi) - 1) t->csi[t->csi_len++] = (char)ch;
                dispatch_csi(t);
                t->state = PS_NORMAL;
            } else if (ch == 0x1b) {
                t->state = PS_ESC;
            } else {
                // Unknown character in CSI; abort
                t->csi_len = 0;
                t->state = PS_NORMAL;
            }
            break;

        case PS_APC:
            if (ch == 0x1b) {
                t->apc_esc_pending = true;
            } else if (t->apc_esc_pending) {
                t->apc_esc_pending = false;
                if (ch == '\\') {
                    t->apc_buf[t->apc_len] = '\0';
                    if (g_kitty_enabled)
                        kitty_handle_apc(t, t->apc_buf, t->apc_len);
                    t->apc_len = 0;
                    t->state = PS_NORMAL;
                } else {
                    if (!t->apc_buf || t->apc_len >= t->apc_cap - 1) {
                        int new_cap = t->apc_cap ? t->apc_cap * 2 : 65536;
                        if (new_cap > 4*1024*1024) {
                            SDL_Log("[APC] buffer exceeded 4MB (apc_len=%d) — aborting sequence\n", t->apc_len);
                            t->apc_len = 0; t->state = PS_NORMAL; break;
                        }
                        t->apc_buf = (char*)realloc(t->apc_buf, new_cap);
                        t->apc_cap = new_cap;
                    }
                    if (t->apc_buf) t->apc_buf[t->apc_len++] = (char)ch;
                }
            } else {
                if (!t->apc_buf || t->apc_len >= t->apc_cap - 1) {
                    int new_cap = t->apc_cap ? t->apc_cap * 2 : 65536;
                    if (new_cap > 4*1024*1024) {
                        SDL_Log("[APC] buffer exceeded 4MB (apc_len=%d) — aborting sequence\n", t->apc_len);
                        t->apc_len = 0; t->state = PS_NORMAL; break;
                    }
                    t->apc_buf = (char*)realloc(t->apc_buf, new_cap);
                    t->apc_cap = new_cap;
                }
                if (t->apc_buf) t->apc_buf[t->apc_len++] = (char)ch;
            }
            break;
        // DCS / PM / SOS — all use the same rule: absorb everything until
        // ST (ESC \) or BEL.  Tmux sends DCS sequences constantly for its
        // passthrough and clipboard protocols.  Without this sink the payload
        // bytes reach PS_NORMAL and get misinterpreted as CSI/text, corrupting
        // the terminal state and eventually crashing.
        //
        // DCS specifically also buffers its content: while the leading
        // parameter string (digits/';') is still being read, bytes are held
        // in dcs_buf. If the first non-parameter byte is 'q', this is a
        // DECSIXEL (Sixel graphics) sequence — buffering continues for the
        // body and gets dispatched to sixel_handle_dcs() at ST/BEL. Any
        // other terminator byte means it's not sixel (e.g. tmux passthrough)
        // and the buffer is dropped immediately, falling back to the
        // original pure-sink behavior so we don't hold megabytes of tmux
        // passthrough data in memory.
        case PS_DCS:
        case PS_PM:
        case PS_SOS:
            if (ch == 0x07) {
                if (t->state == PS_DCS && t->dcs_is_sixel && t->dcs_buf) {
                    SDL_Log("[DCS] dispatching sixel on BEL, body_bytes=%d\n",
                            t->dcs_len - t->dcs_params_len);
                    sixel_handle_dcs(t, t->dcs_buf, t->dcs_params_len,
                                     t->dcs_buf + t->dcs_params_len,
                                     t->dcs_len - t->dcs_params_len);
                }
                t->dcs_len = 0;
                t->state = PS_NORMAL;  // BEL = ST shorthand
            } else if (ch == 0x1b) {
                t->apc_esc_pending = true;  // reuse flag — next char must be '\'
            } else if (t->apc_esc_pending) {
                t->apc_esc_pending = false;
                if (ch == '\\') {
                    if (t->state == PS_DCS && t->dcs_is_sixel && t->dcs_buf) {
                        SDL_Log("[DCS] dispatching sixel on ST, body_bytes=%d\n",
                                t->dcs_len - t->dcs_params_len);
                        sixel_handle_dcs(t, t->dcs_buf, t->dcs_params_len,
                                         t->dcs_buf + t->dcs_params_len,
                                         t->dcs_len - t->dcs_params_len);
                    }
                    t->dcs_len = 0;
                    t->state = PS_NORMAL;
                }
                // else: not ST, keep sinking
            } else if (t->state == PS_DCS) {
                if (!t->dcs_determined) {
                    if ((ch >= '0' && ch <= '9') || ch == ';') {
                        if (!t->dcs_buf || t->dcs_len >= t->dcs_cap - 1) {
                            int new_cap = t->dcs_cap ? t->dcs_cap * 2 : 4096;
                            t->dcs_buf = (char*)realloc(t->dcs_buf, new_cap);
                            t->dcs_cap = new_cap;
                        }
                        if (t->dcs_buf) t->dcs_buf[t->dcs_len++] = (char)ch;
                    } else {
                        t->dcs_determined = true;
                        t->dcs_is_sixel   = (ch == 'q');
                        SDL_Log("[DCS] terminator='%c' is_sixel=%d params_bytes=%d\n",
                                ch, t->dcs_is_sixel, t->dcs_len);
                        if (t->dcs_is_sixel) {
                            t->dcs_params_len = t->dcs_len;  // 'q' itself isn't stored
                        } else {
                            if (t->dcs_buf) { free(t->dcs_buf); t->dcs_buf = nullptr; }
                            t->dcs_cap = 0;
                            t->dcs_len = 0;
                        }
                    }
                } else if (t->dcs_is_sixel) {
                    if (!t->dcs_buf || t->dcs_len >= t->dcs_cap - 1) {
                        int new_cap = t->dcs_cap ? t->dcs_cap * 2 : 65536;
                        if (new_cap > 32*1024*1024) {
                            SDL_Log("[Sixel] buffer exceeded 32MB (dcs_len=%d) — aborting sequence\n", t->dcs_len);
                            t->dcs_len = 0; t->dcs_is_sixel = false; t->state = PS_NORMAL;
                            break;
                        }
                        t->dcs_buf = (char*)realloc(t->dcs_buf, new_cap);
                        t->dcs_cap = new_cap;
                    }
                    if (t->dcs_buf) t->dcs_buf[t->dcs_len++] = (char)ch;
                }
                // else: determined not-sixel — pure sink, nothing to do
            }
            break;
        case PS_OSC:
            if (ch == 0x07 || ch == 0x1b) {
                if (t->osc_buf) {
                    t->osc_buf[t->osc_len] = '\0';
                    dispatch_osc(t, t->osc_buf, t->osc_len, ch == 0x07);
                }
                t->osc_len = 0;
                t->state = (ch == 0x1b) ? PS_ESC : PS_NORMAL;
            } else {
                if (t->osc_len + 1 >= t->osc_cap) {
                    int new_cap = t->osc_cap ? t->osc_cap * 2 : 1024;
                    if (new_cap > 16*1024*1024) break;   // cap: drop the excess
                    char *nb = (char*)realloc(t->osc_buf, new_cap);
                    if (!nb) break;
                    t->osc_buf = nb; t->osc_cap = new_cap;
                }
                t->osc_buf[t->osc_len++] = (char)ch;
            }
            break;
        }

        // If the cursor moved, dirty the old and new rows so the cursor
        // is redrawn correctly. Covers arrow keys, cursor positioning,
        // newlines, backspace, tab — anything that changes cur_row or cur_col.
        if (t->cur_row != prev_row || t->cur_col != prev_col) {
            term_dirty_row(t, prev_row);
            term_dirty_row(t, t->cur_row);
        }
    }
}

bool term_sync_active(Terminal *t) {
    if (!t->sync_output) return false;
    if (SDL_GetTicks() - t->sync_start_ms > 200) return false;  // app forgot ?2026l
    return true;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void term_init(Terminal *t) {
    memset(t, 0, sizeof(*t));
    t->cur_fg               = 7;
    t->pty_fd               = -1;
    t->child                = -1;
    t->state                = PS_NORMAL;
    t->cursor_on            = true;
    t->cursor_blink_enabled = true;
    t->cursor_blink_phase   = true;
    t->cursor_shape         = 1;
    t->autowrap             = true;
    t->saved7_fg            = TCOLOR_PALETTE(7);
    t->saved7_bg            = TCOLOR_PALETTE(0);

    if (s_ft_face) {
        FT_Set_Pixel_Sizes(s_ft_face, 0, g_font_size);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, '0');
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT)) {
            t->cell_w = (float)(s_ft_face->glyph->advance.x >> 6);
            t->cell_h = (float)(int)(g_font_size * 1.4f);
        }
    }
    if (t->cell_w < 1) t->cell_w = 10;
    if (t->cell_h < 1) t->cell_h = 20;

    t->sb_cap = SCROLLBACK_LINES;
    t->cols   = TERM_COLS_DEFAULT;
    t->rows   = TERM_ROWS_DEFAULT;
    t->sb_cols = t->cols;  // Initialize scrollback width to match display width

    t->sb_buf = (Cell*)calloc(t->sb_cap * t->sb_cols, sizeof(Cell));
    t->cells  = (Cell*)malloc(sizeof(Cell) * t->cols * t->rows);
    for (int i = 0; i < t->cols * t->rows; i++)
        t->cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};

    t->scroll_top = 0;
    t->scroll_bot = t->rows - 1;

    t->apc_buf         = nullptr;
    t->apc_len         = 0;
    t->apc_cap         = 0;
    t->apc_esc_pending = false;
    t->utf8_cp         = 0;
    t->utf8_remaining  = 0;
    t->dcs_buf         = nullptr;
    t->dcs_len         = 0;
    t->dcs_cap         = 0;
    t->dcs_params_len  = 0;
    t->dcs_is_sixel    = false;
    t->dcs_determined  = false;

    term_dirty_all(t);
    //SDL_Log("[Term] init: %dx%d cells %.0fx%.0f px\n", t->cols, t->rows, t->cell_w, t->cell_h);
}

// Real terminal reset (RIS / the UI "Reset" action) — clears the grid and
// puts attributes/modes back to defaults, but does NOT touch pty_fd/child
// (the shell is still alive and connected) and does NOT resize the grid to
// the compiled-in default. term_init() is for first-time construction only;
// reusing it here previously clobbered pty_fd to -1, which silently killed
// all keyboard input after hitting Reset.
void term_soft_reset(Terminal *t) {
    for (int i = 0; i < t->cols * t->rows; i++)
        t->cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};

    if (t->alt_cells) {
        free(t->alt_cells);
        t->alt_cells = nullptr;
    }
    if (t->in_alt_screen) {
        t->in_alt_screen = false;
        kitty_leave_alt_screen(t);
        sixel_leave_alt_screen(t);
    }

    t->cur_row = t->cur_col = 0;
    t->cur_fg  = TCOLOR_PALETTE(7);
    t->cur_bg  = TCOLOR_PALETTE(0);
    t->cur_attrs = 0;
    t->cur_ul_style = UL_SINGLE;
    t->cur_ul_color = 0;
    t->cur_hidden   = false;
    t->cur_link     = 0;
    t->last_cp      = 0;
    t->sync_output  = false;
    if (t->cursor_default_saved) {
        t->cursor_shape = t->cursor_default_shape;
    }

    t->scroll_top = 0;
    t->scroll_bot = t->rows - 1;

    t->state    = PS_NORMAL;
    t->g0_line_drawing = false;
    t->csi_len  = 0;
    t->osc_len  = 0;
    t->apc_len  = 0;
    t->apc_esc_pending = false;
    t->utf8_remaining = 0;
    t->dcs_len  = 0;
    t->dcs_is_sixel = false;
    t->dcs_determined = false;

    t->cursor_on            = true;
    t->cursor_blink_enabled = t->cursor_default_saved ? t->cursor_default_blink : true;
    t->cursor_blink_phase   = true;
    t->autowrap             = true;
    t->mouse_report         = false;
    t->bracketed_paste      = false;
    t->app_cursor_keys      = false;
    t->mouse_sgr            = false;

    t->saved7_row = t->saved7_col = 0;
    t->saved7_fg  = TCOLOR_PALETTE(7);
    t->saved7_bg  = TCOLOR_PALETTE(0);
    t->saved7_attrs = 0;

    t->sel_active = false;
    t->sel_exists = false;
    t->sb_offset  = 0;

    term_dirty_all(t);
}

void term_free(Terminal *t) {
    if (t->sb_buf) {
        free(t->sb_buf);
        t->sb_buf = nullptr;
    }
    if (t->cells) {
        free(t->cells);
        t->cells = nullptr;
    }
    if (t->alt_cells) {
        free(t->alt_cells);
        t->alt_cells = nullptr;
    }
    if (t->apc_buf) {
        free(t->apc_buf);
        t->apc_buf = nullptr;
    }
    if (t->dcs_buf) {
        free(t->dcs_buf);
        t->dcs_buf = nullptr;
    }
    if (t->osc_buf) {
        free(t->osc_buf);
        t->osc_buf = nullptr;
        t->osc_cap = 0;
    }
}

void term_resize(Terminal *t, int win_w, int win_h) {
    int new_cols = (int)((win_w - 4) / t->cell_w);
    int new_rows = (int)((win_h - 4) / t->cell_h);
    if (new_cols < 2) new_cols = 2;
    if (new_rows < 2) new_rows = 2;
    if (new_cols > TERM_MAX_COLS) new_cols = TERM_MAX_COLS;
    if (new_rows > TERM_MAX_ROWS) new_rows = TERM_MAX_ROWS;
    if (new_cols == t->cols && new_rows == t->rows) return;

    Cell *new_cells = (Cell*)malloc(sizeof(Cell) * new_cols * new_rows);
    for (int i = 0; i < new_cols * new_rows; i++)
        new_cells[i] = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};

    int copy_rows = (t->rows < new_rows) ? t->rows : new_rows;
    int copy_cols = (t->cols < new_cols) ? t->cols : new_cols;
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++)
            new_cells[r * new_cols + c] = CELL(t, r, c);

    free(t->cells);
    t->cells = new_cells;

    if (t->alt_cells) {
        free(t->alt_cells); t->alt_cells = nullptr;
        if (t->in_alt_screen) {
            t->in_alt_screen = false;
            kitty_leave_alt_screen(t);
            sixel_leave_alt_screen(t);
        }
    }

    // Scrollback buffer: width only increases, never decreases
    // This preserves old data when shrinking the window; we just don't display
    // the overflow. Only reallocate if the new width is larger than scrollback width.
    if (new_cols > t->sb_cols && t->sb_buf) {
        // Column count increased — expand scrollback to new width
        Cell *new_sb_buf = (Cell*)calloc(t->sb_cap * new_cols, sizeof(Cell));
        
        // Copy existing scrollback data to new buffer, padding new columns with blanks
        for (int i = 0; i < t->sb_count; i++) {
            Cell *old_row = sb_row(t, i);
            Cell *new_row = new_sb_buf + ((t->sb_head + i) % t->sb_cap) * new_cols;
            memcpy(new_row, old_row, sizeof(Cell) * t->sb_cols);
            // Pad the extra columns with blanks
            for (int c = t->sb_cols; c < new_cols; c++)
                new_row[c] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
        }
        
        free(t->sb_buf);
        t->sb_buf = new_sb_buf;
        t->sb_cols = new_cols;
    }
    // If new_cols <= t->sb_cols, scrollback width stays at sb_cols; overflow is simply not displayed

    t->cols = new_cols;
    t->rows = new_rows;

    if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
    if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;

#ifndef _WIN32
    if (t->pty_fd >= 0) {
        // Report one fewer column/row than we actually render. We still
        // render the full grid — this margin only affects what child
        // processes (top, bash, etc.) believe COLUMNS/LINES to be, so a
        // line padded to the full reported width never reaches our real
        // autowrap boundary and can't trigger a spurious extra newline.
        int report_cols = new_cols > 1 ? new_cols - 1 : new_cols;
        int report_rows = new_rows > 1 ? new_rows - 1 : new_rows;
        struct winsize ws = {
            .ws_row    = (unsigned short)report_rows,
            .ws_col    = (unsigned short)report_cols,
            .ws_xpixel = (unsigned short)(report_cols * (int)t->cell_w),
            .ws_ypixel = (unsigned short)(report_rows * (int)t->cell_h),
        };
        ioctl(t->pty_fd, TIOCSWINSZ, &ws);
    }
#else
    term_pty_resize(new_cols, new_rows);
#endif

    t->scroll_top = 0; t->scroll_bot = new_rows - 1;
    term_dirty_all(t);
    SDL_Log("[Term] resized to %dx%d\n", new_cols, new_rows);
}

// Update cell dimensions based on current g_font_size.
// Called during initialization after settings are loaded but before window is sized,
// and also by term_set_font_size() during normal operation.
void term_update_cell_dims(Terminal *t) {
    if (s_ft_face) {
        FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)g_font_size);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, '0');
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT)) {
            t->cell_w = (float)(s_ft_face->glyph->advance.x >> 6);
            t->cell_h = (float)(int)(g_font_size * 1.4f);
        }
    }
    if (t->cell_w < 1) t->cell_w = 6;
    if (t->cell_h < 1) t->cell_h = 8;
}

void term_set_font_size(Terminal *t, int new_size, int win_w, int win_h) {
    if (new_size < FONT_SIZE_MIN) new_size = FONT_SIZE_MIN;
    if (new_size > FONT_SIZE_MAX) new_size = FONT_SIZE_MAX;
    if (new_size == g_font_size) return;

    g_font_size = new_size;
    term_update_cell_dims(t);
    term_resize(t, win_w, win_h);
    SDL_Log("[Term] font size %d, cell %.0fx%.0f, grid %dx%d\n", g_font_size, t->cell_w, t->cell_h, t->cols, t->rows);
}

// USAGE IN felixterminal.cpp:
// Add this line in the shutdown section (around line 2248, before ft_shutdown()):
//    term_free(&term);
//
// Example:
//    kitty_shutdown();
//    basic_graphics_shutdown();
//    menu_font_shutdown();
//    term_free(&term);  // <-- ADD THIS LINE
//    if (use_ssh) { ... }
