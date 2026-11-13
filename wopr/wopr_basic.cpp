// wopr_basic.cpp — WOPR sub-game wrapper for BASIC

#include "wopr.h"
#include <SDL2/SDL.h>
#include <string>
#include <cstring>
#include <cctype>
#include <setjmp.h>
#include <signal.h>
#include "sound.h"
#include "wopr_basic_compat.h"
#include "basic_print.h"
#include "basic_ns.h"

// ── BASIC C entry points ────────────────────────────────────────────────
// All shim symbols live in namespace WoprBasic (basic_print.cpp).


/*namespace WoprBasic {
    int   basic_main(void);
    extern char    basic_input_buf[];
    extern int     basic_input_ready;
    extern int     g_basic_game_over;
    extern int     g_basic_waiting_input;
    extern int     g_basic_suppress_newline;
    extern jmp_buf basic_exit_jmp;
    void basic_shim_init(void);
    void basic_shim_set_input(char *line);
}*/
using WoprBasic::basic_main;
using WoprBasic::basic_input_buf;
using WoprBasic::basic_input_ready;
using WoprBasic::g_basic_game_over;
using WoprBasic::g_basic_waiting_input;
using WoprBasic::g_basic_suppress_newline;
using WoprBasic::basic_exit_jmp;
using WoprBasic::basic_shim_init;
using WoprBasic::basic_shim_set_input;
using WoprBasic::basic_input_sem;

// ── Line color encoding ───────────────────────────────────────────────────
static char COLOR_PREFIX = '\x01';
static std::string make_colored_line(const std::string &text, uint8_t r, uint8_t g, uint8_t b)
{
    std::string s;
    s += COLOR_PREFIX;
    s += (char)r; s += (char)g; s += (char)b;  // fg
    s += (char)0; s += (char)0; s += (char)0;  // bg (always black)
    s += text;
    return s;
}
static const uint8_t CGA_RGB[16][3] = {
    {  0,   0,   0},{  0,   0, 170},{  0, 170,   0},{  0, 170, 170},
    {170,   0,   0},{170,   0, 170},{170, 170,   0},{170, 170, 170},
    { 85,  85,  85},{ 85,  85, 255},{ 85, 255,  85},{ 85, 255, 255},
    {255,  85,  85},{255,  85, 255},{255, 255,  85},{255, 255, 255},
};

// ── Per-instance state ────────────────────────────────────────────────────
struct basicState {
    SDL_Thread *thread   = nullptr;
    SDL_mutex  *line_mtx = nullptr;
    SDL_sem    *done_sem = nullptr;
    WoprState  *wopr     = nullptr;
    std::string input_buf;
    bool        dead     = false;

    bool        requires_upper = false;
    
    // Command history
    std::vector<std::string> history;
    int                      history_index = -1;  // -1 = not in history, else index into history
    std::string              history_temp;        // temp storage when editing during history navigation
};

static basicState *s_active = nullptr;
static uint8_t     s_fg_r = 0, s_fg_g = 170, s_fg_b = 0;
static std::string s_out_buf;
static std::string s_prompt_buf;
static int         s_cur_row = 1;  /* 1-based virtual cursor row */
static int         s_cur_col = 1;  /* 1-based virtual cursor col */
static int         s_screen_top = 0; /* lines[] index of row 1 after last CLS */
static int         s_prompt_row = 0; /* s_cur_row when flush_partial was called (0=none) */
static uint8_t     s_prompt_r = 0, s_prompt_g = 170, s_prompt_b = 0;
static const int   MAX_WOPR_LINES = 500;

// ── INKEY$ ring buffer ────────────────────────────────────────────────────
static char       s_key_buf[16];
static int        s_key_head = 0, s_key_tail = 0;
static SDL_mutex *s_key_mtx  = nullptr;

// ── Output helpers (in WoprBasic namespace so basic_print/display_ansi can link) ──
BASIC_NS_BEGIN
void wopr_basic_post_key(char c)
{
    if (!s_key_mtx) return;
    SDL_LockMutex(s_key_mtx);

    basicState *zs = s_active;
    if (zs && zs->requires_upper)
        c = (char)std::toupper((unsigned char)c);

    int next = (s_key_tail + 1) % 16;
    if (next != s_key_head) {
        s_key_buf[s_key_tail] = c;
        s_key_tail = next;
    }
    SDL_UnlockMutex(s_key_mtx);
}
static void commit_line(void)
{
    
    if (!s_active || !s_active->wopr) { 
        s_out_buf.clear(); 
        return; 
    }
    SDL_LockMutex(s_active->line_mtx);
    auto &lines = s_active->wopr->lines;
    int target = s_screen_top + s_cur_row - 1;  /* 0-based index for this row */
    std::string colored = make_colored_line(s_out_buf, s_fg_r, s_fg_g, s_fg_b);
    if (target < (int)lines.size()) {
        lines[target] = colored;  /* overwrite existing row (LOCATE went backwards) */
    } else {
        /* Fill any gap with blank lines, then append */
        while ((int)lines.size() < target)
            lines.push_back("");
        lines.push_back(colored);
    }
    
    // FIX: Auto-scroll viewport to keep new output visible
    // When using simple PRINT loops, s_screen_top needs to scroll with output
    // to avoid text scrolling off into unseen buffer space
    if ((int)lines.size() > MAX_WOPR_LINES)
        lines.erase(lines.begin(), lines.begin() + (lines.size() - MAX_WOPR_LINES));
    
    // Assume 24-row display; keep last 24 lines visible
    int display_rows = 24;
    if ((int)lines.size() > display_rows) {
        s_screen_top = (int)lines.size() - display_rows;
        // Clamp s_cur_row to stay within visible display (1-24)
        if (s_cur_row > display_rows) {
            s_cur_row = display_rows;
        }
    }
    SDL_UnlockMutex(s_active->line_mtx);
    s_out_buf.clear();
    s_cur_row++;
    s_cur_col = 1;
}
// CP437 bytes 128-255 -> UTF-8 sequences
static const char *cp437_utf8[128] = {
    "\xc3\x87",  /* 128 U+00C7 */  "\xc3\xbc",  /* 129 U+00FC */
    "\xc3\xa9",  /* 130 U+00E9 */  "\xc3\xa2",  /* 131 U+00E2 */
    "\xc3\xa4",  /* 132 U+00E4 */  "\xc3\xa0",  /* 133 U+00E0 */
    "\xc3\xa5",  /* 134 U+00E5 */  "\xc3\xa7",  /* 135 U+00E7 */
    "\xc3\xaa",  /* 136 U+00EA */  "\xc3\xab",  /* 137 U+00EB */
    "\xc3\xa8",  /* 138 U+00E8 */  "\xc3\xaf",  /* 139 U+00EF */
    "\xc3\xae",  /* 140 U+00EE */  "\xc3\xac",  /* 141 U+00EC */
    "\xc3\x84",  /* 142 U+00C4 */  "\xc3\x85",  /* 143 U+00C5 */
    "\xc3\x89",  /* 144 U+00C9 */  "\xc3\xa6",  /* 145 U+00E6 */
    "\xc3\x86",  /* 146 U+00C6 */  "\xc3\xb4",  /* 147 U+00F4 */
    "\xc3\xb6",  /* 148 U+00F6 */  "\xc3\xb2",  /* 149 U+00F2 */
    "\xc3\xbb",  /* 150 U+00FB */  "\xc3\xb9",  /* 151 U+00F9 */
    "\xc3\xbf",  /* 152 U+00FF */  "\xc3\x96",  /* 153 U+00D6 */
    "\xc3\x9c",  /* 154 U+00DC */  "\xc2\xa2",  /* 155 U+00A2 */
    "\xc2\xa3",  /* 156 U+00A3 */  "\xc2\xa5",  /* 157 U+00A5 */
    "\xe2\x82\xa7",  /* 158 U+20A7 */  "\xc6\x92",  /* 159 U+0192 */
    "\xc3\xa1",  /* 160 U+00E1 */  "\xc3\xad",  /* 161 U+00ED */
    "\xc3\xb3",  /* 162 U+00F3 */  "\xc3\xba",  /* 163 U+00FA */
    "\xc3\xb1",  /* 164 U+00F1 */  "\xc3\x91",  /* 165 U+00D1 */
    "\xc2\xaa",  /* 166 U+00AA */  "\xc2\xba",  /* 167 U+00BA */
    "\xc2\xbf",  /* 168 U+00BF */  "\xe2\x8c\x90",  /* 169 U+2310 */
    "\xc2\xac",  /* 170 U+00AC */  "\xc2\xbd",  /* 171 U+00BD */
    "\xc2\xbc",  /* 172 U+00BC */  "\xc2\xa1",  /* 173 U+00A1 */
    "\xc2\xab",  /* 174 U+00AB */  "\xc2\xbb",  /* 175 U+00BB */
    "\xe2\x96\x91",  /* 176 U+2591 */  "\xe2\x96\x92",  /* 177 U+2592 */
    "\xe2\x96\x93",  /* 178 U+2593 */  "\xe2\x94\x82",  /* 179 U+2502 */
    "\xe2\x94\xa4",  /* 180 U+2524 */  "\xe2\x95\xa1",  /* 181 U+2561 */
    "\xe2\x95\xa2",  /* 182 U+2562 */  "\xe2\x95\x96",  /* 183 U+2556 */
    "\xe2\x95\x95",  /* 184 U+2555 */  "\xe2\x95\xa3",  /* 185 U+2563 */
    "\xe2\x95\x91",  /* 186 U+2551 */  "\xe2\x95\x97",  /* 187 U+2557 */
    "\xe2\x95\x9d",  /* 188 U+255D */  "\xe2\x95\x9c",  /* 189 U+255C */
    "\xe2\x95\x9b",  /* 190 U+255B */  "\xe2\x94\x90",  /* 191 U+2510 */
    "\xe2\x94\x94",  /* 192 U+2514 */  "\xe2\x94\xb4",  /* 193 U+2534 */
    "\xe2\x94\xac",  /* 194 U+252C */  "\xe2\x94\x9c",  /* 195 U+251C */
    "\xe2\x94\x80",  /* 196 U+2500 */  "\xe2\x94\xbc",  /* 197 U+253C */
    "\xe2\x95\x9e",  /* 198 U+255E */  "\xe2\x95\x9f",  /* 199 U+255F */
    "\xe2\x95\x9a",  /* 200 U+255A */  "\xe2\x95\x94",  /* 201 U+2554 */
    "\xe2\x95\xa9",  /* 202 U+2569 */  "\xe2\x95\xa6",  /* 203 U+2566 */
    "\xe2\x95\xa0",  /* 204 U+2560 */  "\xe2\x95\x90",  /* 205 U+2550 */
    "\xe2\x95\xac",  /* 206 U+256C */  "\xe2\x95\xa7",  /* 207 U+2567 */
    "\xe2\x95\xa8",  /* 208 U+2568 */  "\xe2\x95\xa4",  /* 209 U+2564 */
    "\xe2\x95\xa5",  /* 210 U+2565 */  "\xe2\x95\x99",  /* 211 U+2559 */
    "\xe2\x95\x98",  /* 212 U+2558 */  "\xe2\x95\x92",  /* 213 U+2552 */
    "\xe2\x95\x93",  /* 214 U+2553 */  "\xe2\x95\xab",  /* 215 U+256B */
    "\xe2\x95\xaa",  /* 216 U+256A */  "\xe2\x94\x98",  /* 217 U+2518 */
    "\xe2\x94\x8c",  /* 218 U+250C */  "\xe2\x96\x88",  /* 219 U+2588 */
    "\xe2\x96\x84",  /* 220 U+2584 */  "\xe2\x96\x8c",  /* 221 U+258C */
    "\xe2\x96\x90",  /* 222 U+2590 */  "\xe2\x96\x80",  /* 223 U+2580 */
    "\xce\xb1",  /* 224 U+03B1 */  "\xc3\x9f",  /* 225 U+00DF */
    "\xce\x93",  /* 226 U+0393 */  "\xcf\x80",  /* 227 U+03C0 */
    "\xce\xa3",  /* 228 U+03A3 */  "\xcf\x83",  /* 229 U+03C3 */
    "\xc2\xb5",  /* 230 U+00B5 */  "\xcf\x84",  /* 231 U+03C4 */
    "\xce\xa6",  /* 232 U+03A6 */  "\xce\x98",  /* 233 U+0398 */
    "\xce\xa9",  /* 234 U+03A9 */  "\xce\xb4",  /* 235 U+03B4 */
    "\xe2\x88\x9e",  /* 236 U+221E */  "\xcf\x86",  /* 237 U+03C6 */
    "\xce\xb5",  /* 238 U+03B5 */  "\xe2\x88\xa9",  /* 239 U+2229 */
    "\xe2\x89\xa1",  /* 240 U+2261 */  "\xc2\xb1",  /* 241 U+00B1 */
    "\xe2\x89\xa5",  /* 242 U+2265 */  "\xe2\x89\xa4",  /* 243 U+2264 */
    "\xe2\x8c\xa0",  /* 244 U+2320 */  "\xe2\x8c\xa1",  /* 245 U+2321 */
    "\xc3\xb7",  /* 246 U+00F7 */  "\xe2\x89\x88",  /* 247 U+2248 */
    "\xc2\xb0",  /* 248 U+00B0 */  "\xe2\x88\x99",  /* 249 U+2219 */
    "\xc2\xb7",  /* 250 U+00B7 */  "\xe2\x88\x9a",  /* 251 U+221A */
    "\xe2\x81\xbf",  /* 252 U+207F */  "\xc2\xb2",  /* 253 U+00B2 */
    "\xe2\x96\xa0",  /* 254 U+25A0 */  "\xc2\xa0",  /* 255 U+00A0 */
};

void wopr_basic_push_line(char *text)
{
    if (!s_active || !s_active->wopr || !text) {
        return;
    }
    
    unsigned char *p = (unsigned char *)text;
    while (*p) {
        // Parse ANSI escape sequences
        if (*p == '\033') {
            // ESC sequence — look at next char
            if (*(p+1) == '[') {
                // CSI (Control Sequence Introducer)
                unsigned char *seq_start = p;
                p += 2;
                
                // Skip mode modifier '?' if present (e.g. \033[?25h)
                int has_mode = 0;
                if (*p == '?') {
                    has_mode = 1;
                    p++;
                }
                
                // Parse numeric parameters (separated by semicolons)
                int params[16] = {0};
                int param_count = 0;
                
                while (param_count < 16) {
                    int val = 0;
                    while (*p >= '0' && *p <= '9') {
                        val = val * 10 + (*p - '0');
                        p++;
                    }
                    params[param_count++] = val;
                    if (*p != ';') break;
                    p++;  // skip semicolon
                }
                
                // Now *p should be the command letter
                unsigned char cmd = *p;
                p++;
                
                // Handle the command
                if (cmd == 'H' || cmd == 'f') {
                    // Cursor positioning: ESC[row;colH
                    int row = params[0] ? params[0] : 1;
                    int col = params[1] ? params[1] : 1;
                    wopr_basic_locate(row, col);
                } else if (cmd == 'J') {
                    // Erase in display: ESC[2J = clear screen
                    if (params[0] == 2) {
                        wopr_basic_cls();
                    }
                } else if (cmd == 'K') {
                    // Erase in line: ESC[K = clear to end of line
                    // (flush any partial buffer)
                    wopr_basic_flush_partial();
                } else if (cmd == 'h' || cmd == 'l') {
                    // Mode setting: ESC[?25h (show cursor) / ESC[?25l (hide cursor)
                    // We just ignore these for now since WOPR handles cursor rendering
                } else if (cmd == 'm') {
                    // SGR (Select Graphic Rendition): ESC[...;...;...m
                    // Parse color/attribute codes
                    int fg = -1, bg = -1;
                    for (int i = 0; i < param_count; i++) {
                        int p = params[i];
                        if (p == 0) {
                            // Reset to default (white on black)
                            fg = 7; bg = 0;
                        } else if (p == 1) {
                            // Bold (handled by CGA palette index 8+)
                        } else if (p >= 30 && p <= 37) {
                            // Foreground color (30-37)
                            fg = p - 30;
                        } else if (p >= 40 && p <= 47) {
                            // Background color (40-47)
                            bg = p - 40;
                        }
                    }
                    // Apply the color (only if we got a valid FG color)
                    if (fg >= 0) {
                        if (bg < 0) bg = 0;  // default black background
                        wopr_basic_color(fg, bg);
                    }
                }
                // Unhandled escape sequences are just skipped
                continue;
            }
            // Not a CSI. Some old BASIC programs (this codebase's own
            // wizard.bas included) target real hardware terminals like the
            // Heath/Zenith H19, which used single-character escapes outside
            // the ESC[ CSI form. Recognize the common ones here rather than
            // falling through to printing the raw ESC byte as a visible
            // character (which typically renders as a "missing glyph" box
            // or bell-like symbol -- ESC is never meant to be visible).
            if (*(p+1) == 'E') {
                // H19: clear screen + home cursor (like ESC[2J here).
                wopr_basic_cls();
                p += 2;
                continue;
            }
            // Any other unrecognized single-character escape: drop only the
            // ESC byte itself (never render it as visible text) and let the
            // following character continue through the loop normally, in
            // case it's ordinary text rather than part of some other H19
            // code this doesn't specifically recognize.
            p++;
            continue;
        } else if (*p == '\n') {
            commit_line();
            p++;
        } else if (*p == '\t') {
            // GW-BASIC comma separator uses 14-column tab stops
            int spaces = 14 - ((s_cur_col - 1) % 14);
            if (spaces == 0) spaces = 14;
            for (int i = 0; i < spaces; i++) { s_out_buf += ' '; s_cur_col++; }
            p++;
        } else if (*p >= 128) {
            // Convert CP437 high bytes to UTF-8 for the renderer.
            // The UTF-8 sequence counts as one display column.
            s_out_buf += cp437_utf8[*p - 128];
            s_cur_col++;
            p++;
        } else {
            s_out_buf += (char)*p;
            s_cur_col++;
            p++;
        }
    }
    
}

void wopr_basic_flush_partial(void)
{
    if (!s_active || !s_active->wopr || s_out_buf.empty()) return;
    s_prompt_buf = s_out_buf;
    s_prompt_row = s_cur_row;
    s_prompt_r = s_fg_r; s_prompt_g = s_fg_g; s_prompt_b = s_fg_b;
    s_out_buf.clear();
}
void wopr_basic_cls(void)
{
    if (!s_active || !s_active->wopr) return;
    s_out_buf.clear();
    s_prompt_buf.clear();
    s_prompt_row = 0;
    s_cur_row = 1; s_cur_col = 1;
    SDL_LockMutex(s_active->line_mtx);
    s_active->wopr->lines.clear();
    s_screen_top = 0;
    SDL_UnlockMutex(s_active->line_mtx);
}
void wopr_basic_locate(int row, int col)
{
    if (row < 1) row = 1;
    if (col < 1) col = 1;

    // Flush any partial output at the current position first.
    // If we are moving to a different row, commit current line.
    if (row != s_cur_row && !s_out_buf.empty()) {
        commit_line();
    } else {
        wopr_basic_flush_partial();
    }

    if (row > s_cur_row) {
        // Moving forward: emit blank lines to fill the gap.
        while (s_cur_row < row)
            commit_line();
    } else if (row < s_cur_row) {
        // Moving backwards: just reposition. commit_line will overwrite
        // the existing lines[] entry via the s_screen_top + row - 1 index.
        s_cur_row = row;
        s_cur_col = 1;
    }
    // else row == s_cur_row: stay on this row.

    // Now pad to the target column, starting from where we are.
    // If moving backwards on same row we need to seed s_out_buf from
    // the existing line content up to col so we don't lose what's there.
    if (col > s_cur_col) {
        // Seed from existing line if we repositioned backwards
        if (s_out_buf.empty()) {
            SDL_LockMutex(s_active->line_mtx);
            auto &lines = s_active->wopr->lines;
            int target = s_screen_top + s_cur_row - 1;
            if (target < (int)lines.size()) {
                const std::string &existing = lines[target];
                // Strip the  RGB prefix if present
                const char *txt = existing.c_str();
                if (!existing.empty() && (unsigned char)txt[0] == 0x01)
                    txt += 4;
                s_out_buf = txt;
                s_cur_col = (int)s_out_buf.size() + 1;
            }
            SDL_UnlockMutex(s_active->line_mtx);
        }
        while (s_cur_col < col) {
            s_out_buf += ' ';
            s_cur_col++;
        }
    } else {
        s_cur_col = col;
    }
}

void wopr_basic_color(int fg, int bg)
{
    (void)bg;  // WOPR always uses black background
    if (fg < 0 || fg > 15) fg = 7;
    s_fg_r = CGA_RGB[fg][0]; s_fg_g = CGA_RGB[fg][1]; s_fg_b = CGA_RGB[fg][2];
    // Keep the WOPR global terminal color in sync so the prompt line,
    // crawl text, and any lines without an explicit \x01 prefix also
    // use the color selected by BASIC's COLOR statement.
    g_term_r = s_fg_r; g_term_g = s_fg_g; g_term_b = s_fg_b;
}
int wopr_basic_get_key(void)
{
    if (!s_key_mtx) return -1;
    SDL_LockMutex(s_key_mtx);
    int c = -1;
    if (s_key_head != s_key_tail) { c = (unsigned char)s_key_buf[s_key_head]; s_key_head = (s_key_head+1)%16; }
    SDL_UnlockMutex(s_key_mtx);
    return c;
}
BASIC_NS_END

using WoprBasic::wopr_basic_post_key;

// See wopr.h for why wopr_render() (main thread) needs these to safely read
// WoprState::lines while this file's commit_line() etc. (BASIC interpreter
// thread) push_back()/erase()/assign into it concurrently. Global scope to
// match the rest of this file's public wopr_basic_* API -- s_active is a
// plain file-scope static (declared above, before BASIC_NS_BEGIN), so it's
// visible here without qualification.
void wopr_basic_lines_lock(void) {
    if (s_active && s_active->line_mtx) SDL_LockMutex(s_active->line_mtx);
}
void wopr_basic_lines_unlock(void) {
    if (s_active && s_active->line_mtx) SDL_UnlockMutex(s_active->line_mtx);
}

int wopr_basic_get_screen_top(void)
{
    return s_screen_top;
}

int wopr_basic_get_prompt_row(void)
{
    return s_prompt_row;  // 0 if no active prompt
}

bool wopr_basic_is_waiting_input(WoprState *w)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return false;
    return g_basic_waiting_input != 0;
}
const char *wopr_basic_get_prompt(uint8_t *r, uint8_t *g, uint8_t *b)
{
    // Always use the live fg color so COLOR N immediately affects the
    // input cursor even before any text is printed after it.
    *r = s_fg_r; *g = s_fg_g; *b = s_fg_b;
    return s_prompt_buf.c_str();
}

// ── Game thread ───────────────────────────────────────────────────────────
static int basic_thread_fn(void *userdata)
{
    basicState *zs = static_cast<basicState *>(userdata);
    SDL_Log("[basic] thread start");
    if (setjmp(basic_exit_jmp) == 0) {
        basic_main();
    } else {
        SDL_Log("[basic] longjmp from exit_()");
    }
    
    /* Free the program, variables, arrays and DATA (a normal exit has
     * done this already; a longjmp out of a running program hasn't). The
     * interpreter's own clear_program knows its data layout. */
    WoprBasic::clear_program();

    g_basic_game_over = 1;
    SDL_SemPost(zs->done_sem);
    return 0;
}

// ── Enter ─────────────────────────────────────────────────────────────────
void wopr_basic_enter(WoprState *w)
{
    SDL_Log("[basic] enter");
    g_autoload_path[0] = '\0';  /* clear any previous wizard/autoload path */
    basicState *zs = new basicState();
    zs->line_mtx   = SDL_CreateMutex();
    zs->done_sem   = SDL_CreateSemaphore(0);
    zs->wopr = w; w->sub_state = zs; s_active = zs;
    zs->requires_upper = false;
    if (!s_key_mtx) s_key_mtx = SDL_CreateMutex();
    s_key_head = s_key_tail = 0;
    g_basic_game_over = 0; basic_input_ready = 0; basic_input_buf[0] = '\0';
    g_basic_suppress_newline = 0;
    g_break = 0;  /* Reset break flag to ensure clean BASIC interpreter state */
    s_out_buf.clear();
    s_prompt_buf.clear();
    s_cur_row = 1; s_cur_col = 1; s_screen_top = 0; s_prompt_row = 0;
    s_fg_r = 0; s_fg_g = 170; s_fg_b = 0;
    zs->history.clear();
    zs->history_index = -1;
    zs->history_temp.clear();
    basic_shim_init();
    sound_init();   /* audio init on main thread */

    zs->thread = SDL_CreateThreadWithStackSize(basic_thread_fn, "basicThread", 16 * 1024 * 1024, zs);
    if (!zs->thread) {
        w->lines.push_back("  [basic] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}

// ── Update ────────────────────────────────────────────────────────────────
void wopr_basic_update(WoprState *w, double)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return;
    if (SDL_SemTryWait(zs->done_sem) == 0) {
        zs->dead = true;
        w->lines.push_back(""); w->lines.push_back("  -- BASIC SESSION ENDED --");
        w->lines.push_back("  TYPE  LIST GAMES  TO PLAY AGAIN."); w->lines.push_back("");
        // Thread already posted done_sem and is about to return — safe to join.
        if (zs->thread) { SDL_WaitThread(zs->thread, nullptr); zs->thread = nullptr; }
        if (s_active == zs) s_active = nullptr;
        // Transition phase BEFORE freeing zs so wopr_close won't call
        // wopr_basic_free on an already-freed pointer.
        w->phase = WoprPhase::GAME_MENU; w->input_buf.clear();
        w->sub_state = nullptr;
        SDL_DestroyMutex(zs->line_mtx); SDL_DestroySemaphore(zs->done_sem);
        delete zs;
    }
}

// ── Render ────────────────────────────────────────────────────────────────
void wopr_basic_render(WoprState *w, int win_w, int win_h, int cw, int ch, int cols) 
{ 
    (void)w;
    static int last_log_row = -1;
    if (s_active && s_active->wopr && last_log_row != s_cur_row) {
        SDL_Log("[RENDER_DEBUG] render called: s_screen_top=%d s_cur_row=%d total_lines=%zu win_h=%d ch=%d", 
                s_screen_top, s_cur_row, s_active->wopr->lines.size(), win_h, ch);
        last_log_row = s_cur_row;
    }
}

// ── Mousewheel (scroll support) ───────────────────────────────────────────
bool wopr_basic_mousewheel(WoprState *w, int delta)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || !zs->wopr) return false;

    // Allow scrolling through BASIC output history
    const int LINES_PER_TICK = 3;
    zs->wopr->scroll_offset -= delta * LINES_PER_TICK;
    if (zs->wopr->scroll_offset < 0) zs->wopr->scroll_offset = 0;
    return true;
}

// ── Keydown ───────────────────────────────────────────────────────────────
bool wopr_basic_keydown(WoprState *w, SDL_Keycode sym)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return false;

    if (sym == SDLK_ESCAPE) {
        /* ESC sends CTRL+C to break the BASIC interpreter */
        g_break = 1;
        basic_shim_set_input(const_cast<char*>("\n"));
        return true;
    }

    if (sym == SDLK_c) {
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        if (ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL]) {
            g_break = 1; basic_shim_set_input(const_cast<char*>("\n")); return true;
        }
    }

    if (g_basic_waiting_input) {
        switch (sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: {
            std::string typed = zs->input_buf;
            std::string full = s_prompt_buf + typed;
            // Use the live fg color so COLOR N is immediately visible on
            // the echoed line, not the stale snapshot from flush_partial.
            // Write at the same target position commit_line() would use so
            // the BASIC thread's next commit_line() doesn't overwrite this line.
            {
                SDL_LockMutex(zs->line_mtx);
                auto &lines = s_active->wopr->lines;
                int target = s_screen_top + s_cur_row - 1;
                std::string colored = make_colored_line(full, s_fg_r, s_fg_g, s_fg_b);
                if (target < (int)lines.size()) {
                    lines[target] = colored;
                } else {
                    while ((int)lines.size() < target) lines.push_back("");
                    lines.push_back(colored);
                }
                SDL_UnlockMutex(zs->line_mtx);
            }
            s_prompt_buf.clear();
            s_prompt_row = 0;
            s_cur_row++;
            s_cur_col = 1;
            
            // Save non-empty commands to history
            if (!typed.empty()) {
                zs->history.push_back(typed);
            }
            zs->history_index = -1;  // Reset history navigation
            zs->history_temp.clear();
            
            typed += '\n'; basic_shim_set_input(const_cast<char*>(typed.c_str()));
            // Don't advance cursor here; let BASIC handle it
            zs->input_buf.clear(); w->input_buf.clear(); return true;
        }
        case SDLK_BACKSPACE:
            if (!zs->input_buf.empty()) { zs->input_buf.pop_back(); w->input_buf = zs->input_buf; }
            return true;
        case SDLK_UP: {
            // Navigate backwards through history
            if (zs->history.empty()) return true;
            
            if (zs->history_index == -1) {
                // Save current input as temporary
                zs->history_temp = zs->input_buf;
                zs->history_index = (int)zs->history.size() - 1;
            } else if (zs->history_index > 0) {
                zs->history_index--;
            }
            
            if (zs->history_index >= 0 && zs->history_index < (int)zs->history.size()) {
                zs->input_buf = zs->history[zs->history_index];
                w->input_buf = zs->input_buf;
            }
            return true;
        }
        case SDLK_DOWN: {
            // Navigate forwards through history
            if (zs->history.empty() || zs->history_index == -1) return true;
            
            if (zs->history_index < (int)zs->history.size() - 1) {
                zs->history_index++;
                zs->input_buf = zs->history[zs->history_index];
                w->input_buf = zs->input_buf;
            } else {
                // Return to the temporary input (the line being edited)
                zs->history_index = -1;
                zs->input_buf = zs->history_temp;
                zs->history_temp.clear();
                w->input_buf = zs->input_buf;
            }
            return true;
        }
        case SDLK_ESCAPE: return false;
        default: return true;
        }
    } else {
        if (sym == SDLK_ESCAPE)                          { wopr_basic_post_key(27);   return true; }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) { wopr_basic_post_key('\r'); return true; }
        return true;
    }
}

// ── Text input ────────────────────────────────────────────────────────────
void wopr_basic_text(WoprState *w, const char *text)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead || !text) return;

    if (g_basic_waiting_input) {
        for (const char *p = text; *p; ++p) {
            char c = *p;
            if (zs->requires_upper)
                c = (char)std::toupper((unsigned char)c);
            zs->input_buf += c;
        }
        w->input_buf = zs->input_buf;
    } else {
        for (const char *p = text; *p; ++p)
            wopr_basic_post_key(*p);
    }
}

// ── Wizard's Castle entry ─────────────────────────────────────────────────
// The program is embedded as a C array generated by:
//   xxd -n wopr_basic_program -i wizard.bas > wizard_bas.h
// Include it here so it's only compiled into this TU.
#include "wizard_bas.h"

    // g_autoload_path provided via wopr_basic_compat.h

// Thread function that auto-runs the loaded program instead of showing REPL
static int wizard_thread_fn(void *userdata)
{
    basicState *zs = static_cast<basicState *>(userdata);
    SDL_Log("[wizard] thread start");

    // Write embedded bytes to a platform-appropriate temp file
    char tmp[512];
#ifdef _WIN32
    char *tmp_dir = getenv("TEMP");
    if (!tmp_dir) tmp_dir = getenv("TMP");
    if (!tmp_dir) tmp_dir = "C:\\Temp";
    snprintf(tmp, sizeof(tmp), "%s\\wopr_wizard.bas", tmp_dir);
#else
    snprintf(tmp, sizeof(tmp), "/tmp/wopr_wizard.bas");
#endif
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fwrite(wopr_basic_program, 1, wopr_basic_program_len, f);
        fclose(f);
    }

    if (setjmp(basic_exit_jmp) == 0) {
        strncpy(g_autoload_path, tmp, 511);
        g_autoload_path[511] = '\0';
        basic_main();
    } else {
        SDL_Log("[wizard] longjmp from exit_()");
    }
    g_basic_game_over = 1;
    SDL_SemPost(zs->done_sem);
    return 0;
}

void wopr_wizard_enter(WoprState *w)
{
    SDL_Log("[wizard] enter");
    basicState *zs = new basicState();
    zs->line_mtx   = SDL_CreateMutex();
    zs->done_sem   = SDL_CreateSemaphore(0);
    zs->wopr = w; w->sub_state = zs; s_active = zs;
    zs->requires_upper = true;
    if (!s_key_mtx) s_key_mtx = SDL_CreateMutex();
    s_key_head = s_key_tail = 0;
    g_basic_game_over = 0; basic_input_ready = 0; basic_input_buf[0] = '\0';
    g_basic_suppress_newline = 0;
    s_out_buf.clear();
    s_prompt_buf.clear();
    s_cur_row = 1; s_cur_col = 1; s_screen_top = 0; s_prompt_row = 0;
    s_fg_r = 0; s_fg_g = 170; s_fg_b = 0;
    zs->history.clear();
    zs->history_index = -1;
    zs->history_temp.clear();
    basic_shim_init();
    sound_init();

    zs->thread = SDL_CreateThreadWithStackSize(wizard_thread_fn, "wizardThread", 16 * 1024 * 1024, zs);
    if (!zs->thread) {
        w->lines.push_back("  [wizard] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}


void wopr_basic_free(WoprState *w)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs) return;

    if (!zs->dead) {
        // Signal the thread to exit by sending a CTRL+C break signal
        g_break = 1;
        basic_shim_set_input(const_cast<char*>("\n"));
        if (basic_input_sem) {
            SDL_SemPost(basic_input_sem);
        }
    }

    if (zs->thread) {
        // Give the thread up to 500 ms to exit cleanly.
        // sound_shutdown() is intentionally called AFTER we know the
        // thread has stopped (or been detached) to avoid a deadlock
        // where the audio drain waits for BASIC and BASIC waits for us.
        const int TIMEOUT_MS = 500;
        const int STEP_MS    = 10;
        int waited = 0;
        bool joined = false;
        while (waited < TIMEOUT_MS) {
            if (SDL_SemTryWait(zs->done_sem) == 0) {
                SDL_WaitThread(zs->thread, nullptr);
                joined = true;
                break;
            }
            SDL_Delay(STEP_MS);
            waited += STEP_MS;
        }
        if (!joined) {
            SDL_Log("[basic] free: thread did not exit in %d ms, detaching", TIMEOUT_MS);
            SDL_DetachThread(zs->thread);
        }
        zs->thread = nullptr;
    }

    sound_shutdown();  /* after thread is gone so drain cannot deadlock */

    if (s_active == zs) s_active = nullptr;
    SDL_DestroyMutex(zs->line_mtx);
    SDL_DestroySemaphore(zs->done_sem);
    delete zs;
    w->sub_state = nullptr;
}
