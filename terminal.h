#pragma once
#include "gl_terminal.h"  // ATTR_* defines
#include "term_color.h"
#include <stdint.h>
#include <sys/types.h>
#if defined(_MSC_VER) && !defined(__MINGW32__)
#include "msvc_posix_compat.h"  // pid_t isn't in MSVC's sys/types.h
#endif

// Reported to applications via XTVERSION (CSI > q).
#ifndef FELIX_TERM_NAME
#define FELIX_TERM_NAME    "FelixTerminal"
#endif
#ifndef FELIX_TERM_VERSION
#define FELIX_TERM_VERSION "1.0"
#endif

// ============================================================================
// CELL
// ============================================================================

struct Cell {
    uint32_t     cp;
    TermColorVal fg, bg;
    uint8_t      attrs, _pad[3];   // _pad[0] = CELL_F_* flags (see below)
    // Underline color (SGR 58). 0 = none (underline uses the text color);
    // otherwise CELL_UL_COLOR_SET | TermColorVal. Last member on purpose:
    // existing {cp, fg, bg, attrs, {..}} initializers leave it 0.
    uint32_t     ul_color;
};
#define CELL_UL_COLOR_SET   0x80000000u
#define CELL_UL_COLOR(c)    ((TermColorVal)((c)->ul_color & ~CELL_UL_COLOR_SET))

#define CELL(t,r,c) ((t)->cells[(r)*(t)->cols+(c)])

// Double-width characters (CJK, emoji) occupy two cells: the left one holds
// the codepoint and CELL_F_WIDE; the right one has cp == 0 and
// CELL_F_WIDE_TAIL, and is skipped when drawing glyphs or copying text.
// Flags live in _pad[0] so every existing {cp, fg, bg, attrs, {0,0,0}}
// initializer keeps compiling and means "no flags".
#define CELL_F_WIDE       0x01
#define CELL_F_WIDE_TAIL  0x02

static inline bool cell_is_wide(const Cell *c)      { return (c->_pad[0] & CELL_F_WIDE) != 0; }
static inline bool cell_is_wide_tail(const Cell *c) { return (c->_pad[0] & CELL_F_WIDE_TAIL) != 0; }

// Underline style (SGR 4:n / 21), bits 2-4 of _pad[0]. Only meaningful when
// ATTR_UNDERLINE is set; 0 = the classic single line.
#define UL_SINGLE  0
#define UL_DOUBLE  1
#define UL_CURLY   2
#define UL_DOTTED  3
#define UL_DASHED  4
#define CELL_F_HIDDEN  0x20   // SGR 8 (conceal): cell keeps its text but draws blank
#define CELL_UL_SHIFT  2
#define CELL_UL_MASK   (0x7 << CELL_UL_SHIFT)
static inline int cell_ul_style(const Cell *c) { return (c->_pad[0] & CELL_UL_MASK) >> CELL_UL_SHIFT; }
static inline bool cell_is_hidden(const Cell *c) { return (c->_pad[0] & CELL_F_HIDDEN) != 0; }

// OSC 8 hyperlink id, stored in _pad[1..2] (0 = not a link).
// term_link_uri() maps it back to the URI.
static inline uint16_t cell_link(const Cell *c) { return (uint16_t)(c->_pad[1] | (c->_pad[2] << 8)); }
const char *term_link_uri(uint16_t id);   // nullptr if unknown

// ============================================================================
// TERMINAL
// ============================================================================

typedef enum { PS_NORMAL, PS_ESC, PS_CSI, PS_OSC, PS_CHARSET, PS_APC, PS_DCS, PS_PM, PS_SOS } ParseState;

// One line of the scrollback, kept compact (see sb_encode in terminal.cpp):
// runs of cells that share colors and attributes, each run with only the
// fields that differ from the one before, its characters as varints (ASCII
// is a byte), the blanks at the end left out. sb_row() turns a line back into
// cells when it's needed.
struct SbLine {
    uint8_t      *data;        // nullptr for a blank line
    uint32_t      len;
};

struct Terminal {
    Cell         *cells;
    int           cols, rows;
    int           cur_row, cur_col;
    TermColorVal  cur_fg, cur_bg;
    uint8_t       cur_attrs;
    uint8_t       cur_ul_style;   // UL_* for newly written cells
    uint32_t      cur_ul_color;   // 0 or CELL_UL_COLOR_SET | TermColorVal
    bool          cur_hidden;     // SGR 8 conceal active
    uint16_t      cur_link;       // OSC 8 hyperlink id for new cells (0 = none)
    uint32_t      last_cp;        // last printed character, for REP (CSI b)
    ParseState    state;
    char          charset_slot;   // which Gn ('(' ')' '*' '+') a PS_CHARSET byte designates
    bool          g0_line_drawing; // true if G0 currently designated as DEC special graphics
    char          csi[256];
    int           csi_len;
    char          osc[512];       // (legacy, unused — OSC now goes to osc_buf)
    int           osc_len;        // bytes collected in osc_buf
    char         *osc_buf;        // growable OSC buffer (OSC 52 clipboard
    int           osc_cap;        //  payloads can be megabytes)
    // UTF-8 decode state — persists across term_feed() calls since a
    // multi-byte sequence can be split across separate reads.
    uint32_t      utf8_cp;
    int           utf8_remaining;   // continuation bytes still expected
    // APC buffer — for Kitty graphics protocol (ESC _ ... ESC \)
    char         *apc_buf;
    int           apc_len;
    int           apc_cap;
    bool          apc_esc_pending;  // saw ESC inside APC, waiting for '\' to complete ST
    // DCS buffer — for Sixel graphics (ESC P ... ESC \) and passthrough sinks
    // (tmux, etc). Params (before the sixel 'q' introducer) and body are
    // split at dispatch time in term_feed().
    char         *dcs_buf;
    int           dcs_len;
    int           dcs_cap;
    int           dcs_params_len; // bytes in dcs_buf before the sixel body starts
    bool          dcs_is_sixel;    // set once we see the 'q' introducer
    bool          dcs_determined;  // true once we've decided sixel vs. plain sink
    int           pty_fd;
    pid_t         child;
    float         cell_w, cell_h;
    double        blink;
    double        cursor_blink;
    bool          cursor_on;            // DECTCEM (?25h/l): app wants the cursor shown
    bool          cursor_blink_enabled;
    bool          cursor_blink_phase;   // blink timer: true = lit. Kept separate from
                                        // cursor_on so blinking never un-hides a hidden
                                        // cursor, and stopping the blink can't freeze
                                        // it in the "off" phase.
    int           cursor_shape;        // 0=block, 1=underline bar, 2=beam
    // DECSCUSR "CSI 0 SP q" restores the user's own cursor style, captured
    // the first time an application changes it.
    bool          cursor_default_saved;
    int           cursor_default_shape;
    bool          cursor_default_blink;
    bool          autowrap;
    bool          mouse_report;
    bool          bracketed_paste;
    bool          app_cursor_keys;
    bool          mouse_sgr;
    // Synchronized output (?2026): while set, the app is mid-frame and the
    // screen shouldn't be redrawn. term_sync_active() adds a safety timeout.
    bool          sync_output;
    uint32_t      sync_start_ms;
    // ESC 7/8 saved cursor
    int           saved7_row, saved7_col;
    TermColorVal  saved7_fg, saved7_bg;
    uint8_t       saved7_attrs;
    // Scroll region (DECSTBM, 0-based inclusive)
    int           scroll_top, scroll_bot;
    // Alternate screen
    Cell         *alt_cells;
    int           saved_cur_row, saved_cur_col;
    TermColorVal  saved_cur_fg, saved_cur_bg;
    uint8_t       saved_cur_attrs;
    bool          in_alt_screen;
    // Scrollback: a ring of compact lines (SbLine), and the few of them
    // turned back into cells lately (sb_row): about two screens' worth, each
    // sb_cols wide, keyed by line number, so a row handed out stays good
    // while a screen is drawn.
    SbLine       *sb_lines;    // sb_cap of them
    int           sb_cap;
    int           sb_cols;     // Width of scrollback rows as sb_row gives them (never shrinks, only grows)
    int           sb_head;
    int           sb_count;
    long long     sb_total;    // lines ever pushed: numbers them for the cache
    Cell         *sb_cache;    // sb_cache_n rows of sb_cache_cols
    long long    *sb_cache_line;   // the line in each (-1: none)
    int           sb_cache_n, sb_cache_cols;
    int           sb_offset;   // 0=live, N=N rows back
    // Selection
    int           sel_start_row, sel_start_col;
    int           sel_end_row,   sel_end_col;
    bool          sel_active;
    bool          sel_exists;

    // Per-row dirty flags — set by terminal.cpp on any cell write,
    // cleared by term_render after each row is drawn.
    // Use uint8_t array; TERM_MAX_ROWS is 256 so this is 256 bytes.
    uint8_t       dirty_rows[TERM_MAX_ROWS];
    bool          all_dirty;   // when true, skip per-row check and redraw everything
};

// ============================================================================
// DIRTY ROW HELPERS
// ============================================================================

// Should the cursor be drawn this frame?
static inline bool term_cursor_visible(const Terminal *t) {
    return t->cursor_on && (!t->cursor_blink_enabled || t->cursor_blink_phase);
}

static inline void term_dirty_row(Terminal *t, int row) {
    if (row >= 0 && row < t->rows) t->dirty_rows[row] = 1;
}
static inline void term_dirty_rows(Terminal *t, int top, int bot) {
    for (int r = top; r <= bot && r < t->rows; r++) t->dirty_rows[r] = 1;
}
static inline void term_dirty_all(Terminal *t) {
    t->all_dirty = true;
}
static inline bool term_row_is_dirty(Terminal *t, int row) {
    return t->all_dirty || (row >= 0 && row < t->rows && t->dirty_rows[row]);
}
static inline void term_clear_dirty(Terminal *t) {
    t->all_dirty = false;
    for (int r = 0; r < t->rows; r++) t->dirty_rows[r] = 0;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void term_init(Terminal *t);
// RIS (ESC c) and the UI "Reset" action: clears the screen/attributes back
// to defaults WITHOUT touching pty_fd/child or reallocating at the wrong
// size — unlike term_init(), which is for first-time setup only.
void term_soft_reset(Terminal *t);
void term_resize(Terminal *t, int win_w, int win_h);
void term_set_font_size(Terminal *t, int new_size, int win_w, int win_h);
void term_update_cell_dims(Terminal *t);  // Update cell dimensions without resizing grid
void term_newline(Terminal *t);  // advance cursor one line, scrolling if needed
void term_free(Terminal *t);
// ============================================================================
// VT100 PARSER
// ============================================================================

void term_feed(Terminal *t, const char *buf, int len);

// True while an app holds synchronized output (?2026h) — skip redrawing the
// terminal so a half-drawn frame is never shown. Gives up after 200 ms in
// case the app never sends ?2026l.
bool term_sync_active(Terminal *t);

// Kitty graphics protocol — disable for SSH sessions to prevent APC crashes.
extern bool g_kitty_enabled;

// ============================================================================
// SCROLLBACK ACCESSORS
// ============================================================================

Cell* sb_row(Terminal *t, int idx);
Cell* vcell(Terminal *t, int vrow, int col);
