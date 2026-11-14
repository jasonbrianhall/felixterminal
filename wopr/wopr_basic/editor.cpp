/*
 * editor.cpp — a full-screen editor in the style of MS-DOS QBasic.
 *
 * Blue editing window, a menu bar (File Edit Search Run Options Help) that
 * drops down with Alt, F10 or Alt+letter, dialog boxes, keywords
 * capitalized as you leave a line, F5 to run and Shift+F5 to start over,
 * and F6 (File > Command Line) for the classic Ok prompt, where EDIT comes
 * back here.
 *
 * Everything is drawn through display.h into a shadow screen and only the
 * cells that changed are sent, so it works the same on a terminal, in the
 * SDL window, on MS-DOS and on bare metal. Programs run from the editor are
 * loaded with load_text(), exactly as LOAD would load the same file.
 */
#include "editor.h"

#ifdef BASIC_HAS_EDITOR

#include "basic.h"
#include "display.h"
#ifdef USE_SDL_WINDOW
#include "basic_gfx.h"
#endif
#include <string>
#include <vector>
#include <setjmp.h>

BASIC_NS_BEGIN

extern int     g_run_error_pc;          /* main.cpp */
extern jmp_buf g_parse_error_jmp;       /* expr.cpp */
extern int     g_parse_error_active;

/* ================================================================
 * Shadow screen
 * ================================================================ */
/* Line-drawing and other symbols, drawn as CP437 on the PC-style displays
 * and as Unicode on a terminal. */
enum {
    G_H = 0x100, G_V, G_TL, G_TR, G_BL, G_BR, G_LT, G_RT,
    G_SHADE, G_BLOCK, G_UP, G_DOWN, G_LEFT, G_RIGHT, G_CHECK
};

struct Cell {
    unsigned short ch;
    unsigned char  at;      /* fg | bg << 4 */
    bool operator!=(const Cell &o) const { return ch != o.ch || at != o.at; }
};

static int  W = 80, H = 25;
static std::vector<Cell> scr, shown;
static bool full_redraw = true;
static int  cur_at = -1;                /* colour last sent to the display */

static inline unsigned char A(int fg, int bg) { return (unsigned char)((fg & 15) | (bg & 15) << 4); }

/* QBasic's colours */
#define AT_TEXT    A(7, 1)              /* the editing window */
#define AT_SEL     A(1, 7)              /* selected text */
#define AT_BAR     A(0, 7)              /* menu bar, menus, dialogs */
#ifdef USE_SDL_WINDOW
#define AT_HOT     A(15, 7)             /* a menu's / item's hot key: QBasic's bright white */
#else
#define AT_HOT     A(4, 7)              /* a terminal's grey is near white: red reads */
#endif
#define AT_BARSEL  A(7, 0)              /* the highlighted menu / item */
#define AT_HOTSEL  A(15, 0)
#define AT_GREY    A(8, 7)              /* an item that can't be used now */
#define AT_STATUS  A(15, 3)             /* the status line */
#define AT_SHADOW  A(8, 0)
#define AT_FIELD   A(0, 7)
#define AT_FIELDF  A(15, 0)             /* the text field with the focus */

static void screen_size(void) {
    int w = display_get_width(), h = display_get_rows();
    if (w < 20) w = 20;
    if (h < 8)  h = 8;
    if (w > 400) w = 400;
    if (h > 200) h = 200;
    if (w != W || h != H || (int)scr.size() != w * h) {
        W = w; H = h;
        scr.assign((size_t)(W * H), Cell{ ' ', AT_TEXT });
        shown.assign((size_t)(W * H), Cell{ 0, 0 });
        full_redraw = true;
    }
}

static void put(int x, int y, unsigned short ch, unsigned char at) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    scr[(size_t)(y * W + x)] = Cell{ ch, at };
}

static void text(int x, int y, const char *s, unsigned char at) {
    for (; *s; s++, x++) put(x, y, (unsigned char)*s, at);
}

static void text(int x, int y, const std::string &s, unsigned char at) { text(x, y, s.c_str(), at); }

/* Text with &x marking a hot key, drawn in hot. */
static void text_hot(int x, int y, const char *s, unsigned char at, unsigned char hot) {
    for (; *s; s++) {
        if (*s == '&' && s[1]) { s++; put(x++, y, (unsigned char)*s, hot); }
        else put(x++, y, (unsigned char)*s, at);
    }
}

static int hot_len(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s != '&') n++;
    return n;
}

static int hot_char(const char *s) {
    const char *a = strchr(s, '&');
    return a && a[1] ? toupper((unsigned char)a[1]) : 0;
}

static void fill(int x, int y, int w, int h, unsigned short ch, unsigned char at) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put(x + i, y + j, ch, at);
}

/* The shadow a box casts: two columns to its right, one row below. */
static void shadow(int x, int y, int w, int h) {
    for (int j = y + 1; j <= y + h; j++)
        for (int i = x + w; i < x + w + 2; i++)
            if (i >= 0 && j >= 0 && i < W && j < H) scr[(size_t)(j * W + i)].at = AT_SHADOW;
    for (int i = x + 2; i < x + w; i++)
        if (i >= 0 && y + h < H) scr[(size_t)((y + h) * W + i)].at = AT_SHADOW;
}

static void box(int x, int y, int w, int h, unsigned char at) {
    fill(x, y, w, h, ' ', at);
    for (int i = x + 1; i < x + w - 1; i++) { put(i, y, G_H, at); put(i, y + h - 1, G_H, at); }
    for (int j = y + 1; j < y + h - 1; j++) { put(x, j, G_V, at); put(x + w - 1, j, G_V, at); }
    put(x, y, G_TL, at); put(x + w - 1, y, G_TR, at);
    put(x, y + h - 1, G_BL, at); put(x + w - 1, y + h - 1, G_BR, at);
}

static void hsep(int x, int y, int w, unsigned char at) {
    put(x, y, G_LT, at);
    for (int i = x + 1; i < x + w - 1; i++) put(i, y, G_H, at);
    put(x + w - 1, y, G_RT, at);
}

static void glyph(std::string &out, unsigned short ch) {
#ifdef USE_SDL_WINDOW
    static const unsigned char cp437[] = {
        0xC4, 0xB3, 0xDA, 0xBF, 0xC0, 0xD9, 0xC3, 0xB4,
        0xB0, 0xDB, 0x18, 0x19, 0x11, 0x10, 0xFB };
    if (ch >= 0x100) { out += (char)cp437[ch - 0x100]; return; }
    if (ch < 0x20) { out += '?'; return; }
    out += (char)ch;
#else
    static const char *utf8[] = {
        "\xE2\x94\x80", "\xE2\x94\x82", "\xE2\x94\x8C", "\xE2\x94\x90",
        "\xE2\x94\x94", "\xE2\x94\x98", "\xE2\x94\x9C", "\xE2\x94\xA4",
        "\xE2\x96\x91", "\xE2\x96\x88", "\xE2\x86\x91", "\xE2\x86\x93",
        "\xE2\x86\x90", "\xE2\x86\x92", "\xE2\x88\x9A" };
    if (ch >= 0x100) { out += utf8[ch - 0x100]; return; }
    if (ch < 0x20 || ch >= 0x7F) { out += '?'; return; }   /* one cell per byte */
    out += (char)ch;
#endif
}

static void set_colour(unsigned char at) {
    if (at == cur_at) return;
    display_color(at & 15, at >> 4);
    cur_at = at;
}

/* Send the cells that changed since the last flush. The bottom-right cell
 * is never written, so no display scrolls. */
static void flush(void) {
    if (full_redraw) {
        set_colour(AT_STATUS);
        display_cls();
    }
    std::string out;
    for (int y = 0; y < H; y++) {
        int x = 0;
        while (x < W) {
            size_t i = (size_t)(y * W + x);
            bool last = (y == H - 1 && x == W - 1);
            if (last || (!full_redraw && !(scr[i] != shown[i]))) { x++; continue; }
            int x0 = x;
            unsigned char at = scr[i].at;
            out.clear();
            while (x < W) {
                size_t j = (size_t)(y * W + x);
                if ((y == H - 1 && x == W - 1) || scr[j].at != at) break;
                if (!full_redraw && !(scr[j] != shown[j])) break;
                glyph(out, scr[j].ch);
                shown[j] = scr[j];
                x++;
            }
            display_locate(y + 1, x0 + 1);
            set_colour(at);
            display_print((char *)out.c_str());
        }
    }
    full_redraw = false;
}

static void show_cursor_at(int x, int y) {
    if (x >= 0 && y >= 0 && x < W && y < H) {
        display_locate(y + 1, x + 1);
        display_cursor(1);
    } else {
        display_cursor(0);
    }
}

/* ================================================================
 * Keys
 * ================================================================ */
#define K_F(n)        KEY_EXT(SCAN_F1 + (n) - 1)
#define K_HOME        KEY_EXT(SCAN_HOME)
#define K_END         KEY_EXT(SCAN_END)
#define K_PGUP        KEY_EXT(SCAN_PGUP)
#define K_PGDN        KEY_EXT(SCAN_PGDN)
#define K_INS         KEY_EXT(SCAN_INS)
#define K_DEL         KEY_EXT(SCAN_DEL)
#define K_CTRL_LEFT   KEY_EXT(115)
#define K_CTRL_RIGHT  KEY_EXT(116)
#define K_CTRL_END    KEY_EXT(117)
#define K_CTRL_PGDN   KEY_EXT(118)
#define K_CTRL_HOME   KEY_EXT(119)
#define K_CTRL_PGUP   KEY_EXT(132)
#define K_CTRL_INS    KEY_EXT(146)
#define K_SHIFT_TAB   KEY_EXT(15)
#define CTRL(c)       ((c) - 'A' + 1)

/* Alt+letter arrives as CHR$(0) + the letter key's scan code. */
static int alt_letter(int k) {
    static const char rows[] = "QWERTYUIOP\0\0\0\0ASDFGHJKL\0\0\0\0\0ZXCVBNM";
    if ((k & ~0xFF) != 0x1100) return 0;
    int sc = k & 0xFF;
    if (sc >= 16 && sc < 16 + (int)sizeof rows - 1) return rows[sc - 16];
    return 0;
}

/* The mouse (display.h KEY_MOUSE), as the last event left it: the text
 * cell it's over, the buttons held, and what changed. */
static int m_x, m_y, m_btn, m_wheel, m_press, m_release;

static void mouse_decode(int k) {
    int prev = m_btn;
    m_x = KEY_MOUSE_COL(k); m_y = KEY_MOUSE_ROW(k);
    m_btn = KEY_MOUSE_BUTTONS(k); m_wheel = KEY_MOUSE_WHEEL(k);
    m_press = m_btn & ~prev; m_release = prev & ~m_btn;
}

/* A key or a mouse event (IS_KEY_MOUSE: see m_x...). Every loop that
 * reads keys here handles the mouse, or passes its events over. */
static int getkey_m(void) {
    int k = display_getkey();
    g_break = 0;            /* Ctrl+C is a key here, not a break */
    if (IS_KEY_MOUSE(k)) mouse_decode(k);
    return k;
}


/* ================================================================
 * The program being edited
 * ================================================================ */
struct Snap { std::vector<std::string> lines; int cx, cy; };

static std::vector<std::string> L(1);
static int  cx, cy, top_line, left_col;  /* cursor (cx may be past the end of the line) and scroll */
static bool sel;                         /* a selection from (ax, ay) to the cursor */
static int  ax, ay;
static std::string fname;                /* "" for Untitled */
static bool modified, crlf;
static bool overwrite;
static bool capitalize = true;
static bool auto_indent = true;
static std::string clip;
static std::vector<Snap> undo_stack;
static int  last_kind, last_kind_y = -1;
static int  edited_line = -1;           /* the line typed on, capitalized when the cursor leaves it */
static std::string find_str, change_str;
static unsigned edit_ver = 1, run_ver = 0;
static bool pasting;
static int  last_key;
static std::string status_msg;
static bool quit_now;                   /* the window was closed */
#define TAB_SIZE 4
#define UNDO_MAX 200

enum { K_OTHER = 0, K_TYPE, K_BS, K_DEL_KIND };

/* the editing window */
static int TX(void) { return 1; }
static int TY(void) { return 2; }
static int TW(void) { return W - 2 > 1 ? W - 2 : 1; }
static int TH(void) { return H - 4 > 1 ? H - 4 : 1; }

static int len_of(int y) { return (int)L[(size_t)y].size(); }

static void snapshot(int kind) {
    if (kind != K_OTHER && kind == last_kind && cy == last_kind_y) return;
    undo_stack.push_back(Snap{ L, cx, cy });
    if (undo_stack.size() > UNDO_MAX) undo_stack.erase(undo_stack.begin());
    last_kind = kind;
    last_kind_y = cy;
}

static void edited(void) {
    modified = true;
    edit_ver++;
    edited_line = cy;
}

static void pad_to_cursor(void) {
    if (cx > len_of(cy)) L[(size_t)cy].append((size_t)(cx - len_of(cy)), ' ');
}

static std::string expand_tabs(const std::string &s) {
    std::string r;
    for (char c : s) {
        if (c == '\t') { do r += ' '; while (r.size() % 8); }
        else r += c;
    }
    return r;
}

static void set_text(const char *t, size_t n) {
    L.clear();
    crlf = false;
    std::string cur;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c == '\r') {
            if (i + 1 < n && t[i + 1] == '\n') { crlf = true; i++; }
            L.push_back(expand_tabs(cur)); cur.clear();
        } else if (c == '\n') {
            L.push_back(expand_tabs(cur)); cur.clear();
        } else cur += c;
    }
    if (!cur.empty() || L.empty()) L.push_back(expand_tabs(cur));
    cx = cy = top_line = left_col = 0;
    sel = false;
    undo_stack.clear();
    last_kind = 0;
    edited_line = -1;
    edit_ver++;
}

static std::string get_text(const char *eol = "\n") {
    /* trailing blank lines aren't kept, as QBasic didn't keep them */
    size_t n = L.size();
    while (n > 1 && L[n - 1].empty()) n--;
    std::string t;
    for (size_t i = 0; i < n; i++) {
        std::string s = L[i];
        while (!s.empty() && s.back() == ' ') s.pop_back();
        t += s;
        if (i + 1 < n || !s.empty()) t += eol;
    }
    return t;
}

/* ---- keyword capitalization, as QBasic did when you left a line ---- */
static const char *const KEYWORDS[] = {
    "ABS", "ACCESS", "AND", "ANY", "APPEND", "AS", "ASC", "ATN", "BEEP", "BINARY", "BLOAD",
    "BSAVE", "CALL", "CASE", "CDBL", "CHAIN", "CHDIR", "CHR$", "CINT", "CIRCLE", "CLEAR",
    "CLNG", "CLOSE", "CLS", "COLOR", "COM", "COMMAND$", "COMMON", "CONST", "COS", "CSNG",
    "CSRLIN", "CVD", "CVI", "CVL", "CVS", "DATA", "DATE$", "DECLARE", "DEF", "DEFDBL",
    "DEFINT", "DEFLNG", "DEFSNG", "DEFSTR", "DELAY", "DIM", "DO", "DOUBLE", "DRAW", "ELSE",
    "ELSEIF", "END", "ENVIRON", "ENVIRON$", "EOF", "EQV", "ERASE", "ERDEV", "ERL", "ERR",
    "ERROR", "EXIT", "EXP", "FIELD", "FILEATTR", "FILES", "FIX", "FOR", "FRE", "FREEFILE",
    "FUNCTION", "GET", "GOSUB", "GOTO", "HEX$", "IF", "IMP", "INKEY$", "INP", "INPUT",
    "INPUT$", "INSTR", "INT", "INTEGER", "IOCTL", "IS", "KEY", "KILL", "LBOUND", "LCASE$",
    "LEFT$", "LEN", "LET", "LINE", "LIST", "LOC", "LOCATE", "LOCK", "LOF", "LOG", "LONG",
    "LOOP", "LPOS", "LPRINT", "LSET", "LTRIM$", "MID$", "MKD$", "MKDIR", "MKI$", "MKL$",
    "MKS$", "MOD", "NAME", "NEXT", "NOT", "OCT$", "OFF", "ON", "OPEN", "OPTION", "OR",
    "OUT", "OUTPUT", "PAINT", "PALETTE", "PCOPY", "PEEK", "PEN", "PLAY", "PMAP", "POINT",
    "POKE", "POS", "PRESET", "PRINT", "PSET", "PUT", "RANDOM", "RANDOMIZE", "READ", "REDIM",
    "REM", "RESET", "RESTORE", "RESUME", "RETURN", "RIGHT$", "RMDIR", "RND", "RSET",
    "RTRIM$", "RUN", "SADD", "SCREEN", "SEEK", "SEG", "SELECT", "SGN", "SHARED", "SHELL",
    "SIN", "SINGLE", "SLEEP", "SOUND", "SPACE$", "SPC", "SQR", "STATIC", "STEP", "STICK",
    "STOP", "STR$", "STRIG", "STRING", "STRING$", "SUB", "SWAP", "SYSTEM", "TAB", "TAN",
    "THEN", "TIME$", "TIMER", "TO", "TROFF", "TRON", "TYPE", "UBOUND", "UCASE$", "UNLOCK",
    "UNTIL", "USING", "VAL", "VARPTR", "VARSEG", "VIEW", "WAIT", "WEND", "WHILE", "WIDTH",
    "WINDOW", "WRITE", "XOR", NULL
};

static bool is_keyword(const std::string &w) {
    for (int i = 0; KEYWORDS[i]; i++)
        if (strcasecmp(w.c_str(), KEYWORDS[i]) == 0) return true;
    return false;
}

/* Capitalize the keywords on line y (not in strings or comments). */
static void capitalize_line(int y) {
    if (!capitalize || y < 0 || y >= (int)L.size()) return;
    std::string &s = L[(size_t)y];
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '"') {                                   /* a string */
            i++;
            while (i < n && s[i] != '"') i++;
            i++;
            continue;
        }
        if (c == '\'') return;                            /* a comment */
        if (isdigit((unsigned char)c)) {                  /* a number (1E5, &HFF) */
            while (i < n && isalnum((unsigned char)s[i])) i++;
            continue;
        }
        if (c == '&' && i + 1 < n && isalpha((unsigned char)s[i + 1])) {
            i += 2;
            while (i < n && isalnum((unsigned char)s[i])) i++;
            continue;
        }
        if (!isalpha((unsigned char)c) && c != '_') { i++; continue; }
        size_t b = i;
        while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.')) i++;
        size_t e = i;
        if (i < n && s[i] == '$') e = ++i;                /* LEFT$ */
        std::string w = s.substr(b, e - b);
        if (!is_keyword(w) && e > b && s[e - 1] == '$') { /* a string variable */
            continue;
        }
        if (is_keyword(w)) {
            for (size_t k = b; k < e; k++) s[k] = (char)toupper((unsigned char)s[k]);
            if (strcasecmp(w.c_str(), "REM") == 0) return;
            if (strcasecmp(w.c_str(), "DATA") == 0) {   /* DATA items stay as typed, to the next : */
                bool q = false;
                while (i < n && (q || s[i] != ':')) { if (s[i] == '"') q = !q; i++; }
            }
        }
    }
}

/* ---- selection ---- */
static void sel_range(int &x1, int &y1, int &x2, int &y2) {
    int bx = cx < len_of(cy) ? cx : len_of(cy);
    int a_x = ax < len_of(ay) ? ax : len_of(ay);
    if (ay < cy || (ay == cy && a_x <= bx)) { x1 = a_x; y1 = ay; x2 = bx; y2 = cy; }
    else { x1 = bx; y1 = cy; x2 = a_x; y2 = ay; }
}

static bool sel_empty(void) {
    if (!sel) return true;
    int x1, y1, x2, y2;
    sel_range(x1, y1, x2, y2);
    return x1 == x2 && y1 == y2;
}

static bool in_sel(int x, int y) {
    if (!sel) return false;
    int x1, y1, x2, y2;
    sel_range(x1, y1, x2, y2);
    if (y < y1 || y > y2) return false;
    if (y1 == y2) return x >= x1 && x < x2;
    if (y == y1) return x >= x1 && x <= len_of(y);
    if (y == y2) return x < x2;
    return x <= len_of(y);
}

static std::string sel_text(void) {
    int x1, y1, x2, y2;
    sel_range(x1, y1, x2, y2);
    if (y1 == y2) return L[(size_t)y1].substr((size_t)x1, (size_t)(x2 - x1));
    std::string t = L[(size_t)y1].substr((size_t)x1) + "\n";
    for (int y = y1 + 1; y < y2; y++) t += L[(size_t)y] + "\n";
    t += L[(size_t)y2].substr(0, (size_t)x2);
    return t;
}

static void delete_selection(void) {
    if (sel_empty()) { sel = false; return; }
    int x1, y1, x2, y2;
    sel_range(x1, y1, x2, y2);
    snapshot(K_OTHER);
    L[(size_t)y1] = L[(size_t)y1].substr(0, (size_t)x1) + L[(size_t)y2].substr((size_t)x2);
    L.erase(L.begin() + y1 + 1, L.begin() + y2 + 1);
    cx = x1; cy = y1;
    sel = false;
    edited();
}

/* ---- editing ---- */
static void insert_text(const std::string &t) {
    if (sel) delete_selection();
    snapshot(K_OTHER);
    pad_to_cursor();
    std::string tail = L[(size_t)cy].substr((size_t)cx);
    L[(size_t)cy].erase((size_t)cx);
    int first = cy;
    size_t start = 0;
    for (;;) {
        size_t nl = t.find('\n', start);
        std::string part = t.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        L[(size_t)cy] += part;
        cx = len_of(cy);
        if (nl == std::string::npos) break;
        L.insert(L.begin() + cy + 1, std::string());
        cy++;
        start = nl + 1;
    }
    L[(size_t)cy] += tail;
    for (int y = first; y < cy; y++) capitalize_line(y);
    edited();
}

static void type_char(int c) {
    if (sel) delete_selection();
    snapshot(K_TYPE);
    pad_to_cursor();
    if (overwrite && cx < len_of(cy)) L[(size_t)cy][(size_t)cx] = (char)c;
    else L[(size_t)cy].insert((size_t)cx, 1, (char)c);
    cx++;
    edited();
}

static void new_line(void) {
    if (sel) delete_selection();
    snapshot(K_OTHER);
    std::string &s = L[(size_t)cy];
    int pos = cx < (int)s.size() ? cx : (int)s.size();
    std::string tail = s.substr((size_t)pos);
    s.erase((size_t)pos);
    std::string indent;
    if (auto_indent && !pasting) {
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') i++;
        if (i == s.size()) i = (size_t)(cx < (int)i ? cx : (int)i);  /* a blank line: keep the column */
        indent.assign(i, ' ');
        size_t j = 0;
        while (j < tail.size() && tail[j] == ' ') j++;
        tail.erase(0, j);
    }
    if (tail.empty()) while (!s.empty() && s.back() == ' ') s.pop_back();
    capitalize_line(cy);
    L.insert(L.begin() + cy + 1, indent + tail);
    cy++;
    cx = (int)indent.size();
    edited();
}

static void backspace(void) {
    if (sel && !sel_empty()) { delete_selection(); return; }
    sel = false;
    if (cx > len_of(cy)) { cx--; return; }
    if (cx > 0) {
        snapshot(K_BS);
        L[(size_t)cy].erase((size_t)cx - 1, 1);
        cx--;
        edited();
    } else if (cy > 0) {
        snapshot(K_OTHER);
        cx = len_of(cy - 1);
        L[(size_t)cy - 1] += L[(size_t)cy];
        L.erase(L.begin() + cy);
        cy--;
        edited();
    }
}

static void delete_char(void) {
    if (sel && !sel_empty()) { delete_selection(); return; }
    sel = false;
    if (cx < len_of(cy)) {
        snapshot(K_DEL_KIND);
        L[(size_t)cy].erase((size_t)cx, 1);
        edited();
    } else if (cy + 1 < (int)L.size()) {
        snapshot(K_OTHER);
        pad_to_cursor();
        L[(size_t)cy] += L[(size_t)cy + 1];
        L.erase(L.begin() + cy + 1);
        edited();
    }
}

/* Tab / Shift+Tab: indent or outdent the selected lines, or the current one. */
static void indent_lines(bool out) {
    int y1 = cy, y2 = cy;
    if (sel && !sel_empty()) {
        int x1, x2;
        sel_range(x1, y1, x2, y2);
        if (x2 == 0 && y2 > y1) y2--;
    } else if (!out) {
        /* no selection: Tab inserts spaces to the next tab stop */
        int n = TAB_SIZE - cx % TAB_SIZE;
        if (sel) delete_selection();
        snapshot(K_OTHER);
        pad_to_cursor();
        if (overwrite) cx += n;
        else { L[(size_t)cy].insert((size_t)cx, (size_t)n, ' '); cx += n; edited(); }
        return;
    }
    snapshot(K_OTHER);
    for (int y = y1; y <= y2; y++) {
        std::string &s = L[(size_t)y];
        if (out) {
            int n = 0;
            while (n < TAB_SIZE && n < (int)s.size() && s[(size_t)n] == ' ') n++;
            s.erase(0, (size_t)n);
            if (y == cy) cx = cx > n ? cx - n : 0;
            if (y == ay) ax = ax > n ? ax - n : 0;
        } else if (!s.empty()) {
            s.insert(0, TAB_SIZE, ' ');
            if (y == cy && cx > 0) cx += TAB_SIZE;
            if (y == ay && ax > 0) ax += TAB_SIZE;
        }
    }
    edited();
}

static void undo(void) {
    if (undo_stack.empty()) { status_msg = "Nothing to undo"; return; }
    Snap s = undo_stack.back();
    undo_stack.pop_back();
    L = s.lines;
    cx = s.cx; cy = s.cy;
    if (cy >= (int)L.size()) cy = (int)L.size() - 1;
    sel = false;
    last_kind = 0;
    modified = true;
    edit_ver++;
}

static void copy_sel(void) {
    if (sel_empty()) return;
    clip = sel_text();
    display_clipboard_set(clip.c_str());
}

static void paste(void) {
    std::string t = clip;
    char *sys = display_clipboard_get();
    if (sys) { t = sys; free(sys); }
    std::string clean;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\r') { clean += '\n'; if (i + 1 < t.size() && t[i + 1] == '\n') i++; }
        else clean += t[i];
    }
    if (clean.empty()) return;
    /* tabs expanded line by line */
    std::string out, line;
    for (char c : clean) {
        if (c == '\n') { out += expand_tabs(line) + "\n"; line.clear(); }
        else line += c;
    }
    out += expand_tabs(line);
    insert_text(out);
}

static void cut_line(void) {
    snapshot(K_OTHER);
    clip = L[(size_t)cy] + "\n";
    display_clipboard_set(clip.c_str());
    if (L.size() > 1) L.erase(L.begin() + cy);
    else L[0].clear();
    if (cy >= (int)L.size()) cy = (int)L.size() - 1;
    cx = 0;
    sel = false;
    edited();
}

/* ---- moving ---- */
static void clamp_cursor(void) {
    if (cy < 0) cy = 0;
    if (cy >= (int)L.size()) cy = (int)L.size() - 1;
    if (cx < 0) cx = 0;
    if (cx > 255 && cx > len_of(cy)) cx = len_of(cy) > 255 ? len_of(cy) : 255;
}

static void scroll_to_cursor(void) {
    int th = TH(), tw = TW();
    if (cy < top_line) top_line = cy;
    if (cy >= top_line + th) top_line = cy - th + 1;
    if (top_line < 0) top_line = 0;
    if (cx < left_col) left_col = cx > tw / 2 ? cx - tw / 2 : 0;
    if (cx >= left_col + tw) left_col = cx - tw + tw / 4 + 1;
    if (left_col < 0) left_col = 0;
}

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$'; }

static void word_left(void) {
    if (cx > len_of(cy)) cx = len_of(cy);
    if (cx == 0) { if (cy > 0) { cy--; cx = len_of(cy); } return; }
    const std::string &s = L[(size_t)cy];
    while (cx > 0 && !word_char(s[(size_t)cx - 1])) cx--;
    while (cx > 0 && word_char(s[(size_t)cx - 1])) cx--;
}

static void word_right(void) {
    const std::string &s = L[(size_t)cy];
    if (cx >= len_of(cy)) { if (cy + 1 < (int)L.size()) { cy++; cx = 0; } return; }
    while (cx < len_of(cy) && word_char(s[(size_t)cx])) cx++;
    while (cx < len_of(cy) && !word_char(s[(size_t)cx])) cx++;
}

static void go_line(int y, int x) {
    cy = y; cx = x;
    clamp_cursor();
    scroll_to_cursor();
    /* show it a few lines from the top when it was off the screen */
    if (cy >= TH() / 2 && (cy < top_line + 2 || cy > top_line + TH() - 3)) top_line = cy - TH() / 3;
    if (top_line < 0) top_line = 0;
}

/* ================================================================
 * Menus
 * ================================================================ */
enum Cmd {
    CMD_NONE, CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_CLI, CMD_EXIT,
    CMD_UNDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_CLEAR, CMD_SELALL, CMD_DELLINE,
    CMD_FIND, CMD_FINDNEXT, CMD_CHANGE, CMD_GOTO,
    CMD_START, CMD_CONT,
    CMD_OPT_CAPS, CMD_OPT_INDENT, CMD_OPT_OVR,
    CMD_HELP_KEYS, CMD_HELP_START, CMD_HELP_ABOUT
};

struct MenuItem { const char *label; const char *keys; int cmd; const char *help; };
struct Menu { const char *name; const MenuItem *items; int n; };

static const MenuItem FILE_ITEMS[] = {
    { "&New",            "Ctrl+N",  CMD_NEW,    "Removes currently loaded program from memory" },
    { "&Open...",        "Ctrl+O",  CMD_OPEN,   "Loads new program into memory" },
    { "&Save",           "Ctrl+S",  CMD_SAVE,   "Writes current program to the file it came from" },
    { "Save &As...",     "",        CMD_SAVEAS, "Saves current program with a new name" },
    { "-", "", 0, "" },
    { "Command &Line",   "F6",      CMD_CLI,    "Goes to the Ok prompt (type EDIT to come back)" },
    { "-", "", 0, "" },
    { "E&xit",           "",        CMD_EXIT,   "Exits Felix BASIC" },
};
static const MenuItem EDIT_ITEMS[] = {
    { "&Undo",           "Ctrl+Z",  CMD_UNDO,   "Undoes the last change" },
    { "Cu&t",            "Ctrl+X",  CMD_CUT,    "Deletes selected text and copies it to clipboard" },
    { "&Copy",           "Ctrl+C",  CMD_COPY,   "Copies selected text to clipboard" },
    { "&Paste",          "Ctrl+V",  CMD_PASTE,  "Inserts clipboard contents at the cursor" },
    { "Cl&ear",          "Del",     CMD_CLEAR,  "Deletes selected text without copying it" },
    { "-", "", 0, "" },
    { "Delete &Line",    "Ctrl+Y",  CMD_DELLINE,"Cuts the current line to the clipboard" },
    { "Select &All",     "Ctrl+A",  CMD_SELALL, "Selects the whole program" },
};
static const MenuItem SEARCH_ITEMS[] = {
    { "&Find...",        "Ctrl+F",  CMD_FIND,     "Finds specified text" },
    { "&Repeat Last Find","F3",     CMD_FINDNEXT, "Finds next occurrence of text specified in previous search" },
    { "&Change...",      "Ctrl+R",  CMD_CHANGE,   "Finds and changes specified text" },
    { "-", "", 0, "" },
    { "&Go to Line...",  "Ctrl+G",  CMD_GOTO,     "Moves the cursor to a line number" },
};
static const MenuItem RUN_ITEMS[] = {
    { "&Start",          "Shift+F5",CMD_START,  "Runs current program" },
    { "&Continue",       "F5",      CMD_CONT,   "Continues execution after a break (or starts the program)" },
    { "-", "", 0, "" },
    { "Command &Line",   "F6",      CMD_CLI,    "Goes to the Ok prompt (type EDIT to come back)" },
};
static const MenuItem OPTIONS_ITEMS[] = {
    { "Capitalize &Keywords", "",   CMD_OPT_CAPS,   "Capitalizes BASIC keywords when you leave a line" },
    { "&Auto Indent",    "",        CMD_OPT_INDENT, "Starts a new line at the indentation of the last one" },
    { "&Overtype",       "Ins",     CMD_OPT_OVR,    "Typing replaces characters instead of inserting" },
};
static const MenuItem HELP_ITEMS[] = {
    { "&Keyboard",       "F1",      CMD_HELP_KEYS,  "Editor keys" },
    { "&Getting Started","",        CMD_HELP_START, "Running programs and using the command line" },
    { "-", "", 0, "" },
    { "&About...",       "",        CMD_HELP_ABOUT, "Displays the version of Felix BASIC" },
};

#define NITEMS(a) (int)(sizeof(a) / sizeof((a)[0]))
static const Menu MENUS[] = {
    { "&File",    FILE_ITEMS,    NITEMS(FILE_ITEMS) },
    { "&Edit",    EDIT_ITEMS,    NITEMS(EDIT_ITEMS) },
    { "&Search",  SEARCH_ITEMS,  NITEMS(SEARCH_ITEMS) },
    { "&Run",     RUN_ITEMS,     NITEMS(RUN_ITEMS) },
    { "&Options", OPTIONS_ITEMS, NITEMS(OPTIONS_ITEMS) },
    { "&Help",    HELP_ITEMS,    NITEMS(HELP_ITEMS) },
};
#define NMENUS NITEMS(MENUS)

static bool can_continue(void) { return g_cont_pc >= 0 && run_ver == edit_ver; }

static bool cmd_enabled(int cmd) {
    switch (cmd) {
    case CMD_UNDO:     return !undo_stack.empty();
    case CMD_CUT: case CMD_COPY: case CMD_CLEAR: return !sel_empty();
    case CMD_FINDNEXT: return !find_str.empty();
    case CMD_CONT:     return true;
    default:           return true;
    }
}

static bool cmd_checked(int cmd) {
    switch (cmd) {
    case CMD_OPT_CAPS:   return capitalize;
    case CMD_OPT_INDENT: return auto_indent;
    case CMD_OPT_OVR:    return overwrite;
    }
    return false;
}

static int menu_x(int m) {
    if (m == NMENUS - 1) return W - hot_len(MENUS[m].name) - 3;   /* Help, at the right */
    int x = 2;
    for (int i = 0; i < m; i++) x += hot_len(MENUS[i].name) + 2;
    return x;
}

static void draw_editor(void);
static void draw_status(const char *left);

static int menu_width(int m) {
    int w = 0;
    for (int i = 0; i < MENUS[m].n; i++) {
        const MenuItem &it = MENUS[m].items[i];
        int iw = hot_len(it.label) + (it.keys[0] ? (int)strlen(it.keys) + 3 : 0);
        if (iw > w) w = iw;
    }
    return w + 6;
}

static void draw_menu_bar(int active) {
    fill(0, 0, W, 1, ' ', AT_BAR);
    for (int m = 0; m < NMENUS; m++) {
        int x = menu_x(m);
        bool on = m == active;
        fill(x - 1, 0, hot_len(MENUS[m].name) + 2, 1, ' ', on ? AT_BARSEL : AT_BAR);
        text_hot(x, 0, MENUS[m].name, on ? AT_BARSEL : AT_BAR, on ? AT_HOTSEL : AT_HOT);
    }
}

/* The menu whose name is at column x of the menu bar, or -1. */
static int menu_at(int x) {
    for (int m = 0; m < NMENUS; m++)
        if (x >= menu_x(m) - 1 && x < menu_x(m) + hot_len(MENUS[m].name) + 1) return m;
    return -1;
}

/* The left edge of menu m when it's dropped down. */
static int dropdown_x(int m) {
    int w = menu_width(m), x = menu_x(m) - 1;
    if (x + w + 2 > W) x = W - w - 2;
    if (x < 0) x = 0;
    return x;
}

/* The item of dropped-down menu m at cell (x, y): -1 for none, -2 for
 * the box's frame. */
static int dropdown_item_at(int m, int x, int y) {
    int dx = dropdown_x(m), w = menu_width(m);
    if (x < dx || x >= dx + w || y < 1 || y >= MENUS[m].n + 3) return -1;
    if (x == dx || x == dx + w - 1 || y == 1 || y == MENUS[m].n + 2) return -2;
    return MENUS[m].items[y - 2].label[0] == '-' ? -2 : y - 2;
}

static void draw_dropdown(int m, int item) {
    const Menu &mn = MENUS[m];
    int w = menu_width(m), h = mn.n + 2;
    int x = dropdown_x(m);
    box(x, 1, w, h, AT_BAR);
    shadow(x, 1, w, h);
    for (int i = 0; i < mn.n; i++) {
        const MenuItem &it = mn.items[i];
        int y = 2 + i;
        if (it.label[0] == '-') { hsep(x, y, w, AT_BAR); continue; }
        bool en = cmd_enabled(it.cmd), on = i == item;
        unsigned char at = on ? AT_BARSEL : en ? AT_BAR : AT_GREY;
        unsigned char hot = on ? AT_HOTSEL : en ? AT_HOT : AT_GREY;
        fill(x + 1, y, w - 2, 1, ' ', at);
        if (cmd_checked(it.cmd)) put(x + 1, y, G_CHECK, at);
        text_hot(x + 2, y, it.label, at, hot);
        if (it.keys[0]) text(x + w - 2 - (int)strlen(it.keys), y, it.keys, at);
    }
}

static int next_item(int m, int i, int dir) {
    int n = MENUS[m].n;
    for (int t = 0; t < n; t++) {
        i = (i + dir + n) % n;
        if (MENUS[m].items[i].label[0] != '-') return i;
    }
    return 0;
}

/* The menu bar: m is the menu to start on, open whether it's dropped down.
 * Returns the command chosen, or CMD_NONE. */
static int menu_loop(int m, bool open) {
    int item = 0;
    for (;;) {
        screen_size();
        draw_editor();
        draw_menu_bar(m);
        if (open) {
            draw_dropdown(m, item);
            draw_status(MENUS[m].items[item].help);
        } else {
            draw_status("Use the arrow keys or the highlighted letter; Enter opens a menu, Esc leaves");
        }
        flush();
        display_cursor(0);
        int k = getkey_m();
        if (k < 0) { quit_now = true; return CMD_NONE; }
        if (IS_KEY_MOUSE(k)) {
            /* Press on a menu's name to drop it down (again to put it away),
             * then click an item, or hold the button, slide to the item and
             * let go there. A press anywhere else leaves the menus. */
            if (m_wheel) continue;
            int bar = m_y == 0 ? menu_at(m_x) : -1;
            int at = open ? dropdown_item_at(m, m_x, m_y) : -1;
            if (m_press & 1) {
                if (bar >= 0) {
                    if (bar == m && open) return CMD_NONE;
                    m = bar; open = true; item = 0;
                    continue;
                }
                if (at == -1) return CMD_NONE;
            }
            if (m_btn & 1) {
                if (bar >= 0 && (bar != m || !open)) { m = bar; open = true; item = 0; }
                else if (at >= 0) item = at;
            }
            if ((m_release & 1) && at >= 0) {
                item = at;
                const MenuItem &it = MENUS[m].items[item];
                if (cmd_enabled(it.cmd)) return it.cmd;
            }
            continue;
        }
        int base = k & ~KEY_SHIFT;
        int al = alt_letter(base);
        if (k == 27 || base == K_F(10) || k == KEY_ALT_ALONE) return CMD_NONE;
        if (base == KEY_LEFT)  { m = (m + NMENUS - 1) % NMENUS; item = 0; continue; }
        if (base == KEY_RIGHT) { m = (m + 1) % NMENUS; item = 0; continue; }
        if (!open) {
            if (k == '\r' || base == KEY_DOWN || base == KEY_UP) { open = true; item = 0; continue; }
            int c = al ? al : (k < 256 ? toupper(k) : 0);
            for (int i = 0; c && i < NMENUS; i++)
                if (hot_char(MENUS[i].name) == c) { m = i; open = true; item = 0; break; }
            continue;
        }
        if (base == KEY_DOWN) { item = next_item(m, item, 1); continue; }
        if (base == KEY_UP)   { item = next_item(m, item, -1); continue; }
        if (base == K_HOME)   { item = next_item(m, MENUS[m].n - 1, 1); continue; }
        if (base == K_END)    { item = next_item(m, 0, -1); continue; }
        if (k == '\r') {
            const MenuItem &it = MENUS[m].items[item];
            if (cmd_enabled(it.cmd)) return it.cmd;
            continue;
        }
        if (al) {   /* Alt+letter: another menu */
            for (int i = 0; i < NMENUS; i++)
                if (hot_char(MENUS[i].name) == al) { m = i; item = 0; break; }
            continue;
        }
        if (k > 32 && k < 256) {
            int c = toupper(k);
            for (int i = 0; i < MENUS[m].n; i++) {
                const MenuItem &it = MENUS[m].items[i];
                if (it.label[0] != '-' && hot_char(it.label) == c && cmd_enabled(it.cmd)) return it.cmd;
            }
        }
    }
}

/* ================================================================
 * Drawing the editor
 * ================================================================ */
static std::string base_name(const std::string &p) {
    size_t s = p.find_last_of("/\\:");
    return s == std::string::npos ? p : p.substr(s + 1);
}

/* The status line's keys, which can be clicked too */
#define STATUS_KEYS "<F1=Help> <F10=Menus> <F5=Run> <F6=Command Line> <Ctrl+S=Save>"
static bool status_keys_shown;

static void draw_status(const char *left) {
    fill(0, H - 1, W, 1, ' ', AT_STATUS);
    char right[48];
    snprintf(right, sizeof right, "%c %s%05d:%03d ", ' ', overwrite ? "OVR " : "", cy + 1, cx + 1);
    int rw = (int)strlen(right);
    put(W - rw - 1, H - 1, G_V, A(0, 3));
    text(W - rw, H - 1, right, A(0, 3));
    std::string l = left;
    int room = W - rw - 3;
    if ((int)l.size() > room) l = l.substr(0, room > 0 ? (size_t)room : 0);
    text(1, H - 1, l, AT_STATUS);
}

static void draw_editor(void) {
    draw_menu_bar(-1);

    /* the window and its title */
    int th = TH(), tw = TW();
    box(0, 1, W, H - 2, AT_TEXT);
    std::string title = " " + (fname.empty() ? std::string("Untitled") : base_name(fname)) + " ";
    if ((int)title.size() > W - 4) title = title.substr(0, (size_t)(W - 4));
    text((W - (int)title.size()) / 2, 1, title, A(1, 7));

    /* the text */
    for (int r = 0; r < th; r++) {
        int y = top_line + r;
        for (int c = 0; c < tw; c++) {
            int x = left_col + c;
            unsigned short ch = ' ';
            unsigned char at = AT_TEXT;
            if (y < (int)L.size()) {
                if (x < len_of(y)) ch = (unsigned char)L[(size_t)y][(size_t)x];
                if (in_sel(x, y)) at = AT_SEL;
            }
            put(TX() + c, TY() + r, ch, at);
        }
    }

    /* vertical scroll bar on the right edge */
    if (th >= 3) {
        int x = W - 1;
        put(x, TY(), G_UP, AT_BAR);
        put(x, TY() + th - 1, G_DOWN, AT_BAR);
        for (int j = 1; j < th - 1; j++) put(x, TY() + j, G_SHADE, AT_BAR);
        int track = th - 2, n = (int)L.size();
        int pos = n > 1 ? (int)((long long)cy * (track - 1) / (n - 1)) : 0;
        put(x, TY() + 1 + pos, ' ', A(0, 0));
    }
    /* horizontal scroll bar along the bottom */
    if (tw >= 3) {
        int y = H - 2;
        put(1, y, G_LEFT, AT_BAR);
        put(W - 2, y, G_RIGHT, AT_BAR);
        for (int i = 2; i < W - 2; i++) put(i, y, G_SHADE, AT_BAR);
        int track = W - 4;
        int pos = (int)((long long)(cx < 255 ? cx : 255) * (track - 1) / 255);
        put(2 + pos, y, ' ', A(0, 0));
    }

    status_keys_shown = status_msg.empty();
    if (!status_keys_shown) draw_status(status_msg.c_str());
    else draw_status(STATUS_KEYS);
}

/* ================================================================
 * Dialog boxes
 * ================================================================ */
static void center_box(int w, int h, const char *title, int &x, int &y) {
    if (w > W - 2) w = W - 2;
    if (h > H - 2) h = H - 2;
    x = (W - w) / 2;
    y = (H - h) / 2;
    box(x, y, w, h, AT_BAR);
    shadow(x, y, w, h);
    if (title && *title) {
        std::string t = std::string(" ") + title + " ";
        text(x + (w - (int)t.size()) / 2, y, t, AT_BAR);
    }
}

static int buttons_width(const std::vector<std::string> &b) {
    int w = 0;
    for (size_t i = 0; i < b.size(); i++) w += (int)b[i].size() + 4 + (i ? 3 : 0);
    return w;
}

/* Buttons centred on row y of a box at x, w wide; focus < 0: none. */
static void draw_buttons(int x, int y, int w, const std::vector<std::string> &b, int focus) {
    int bx = x + (w - buttons_width(b)) / 2;
    for (size_t i = 0; i < b.size(); i++) {
        bool on = (int)i == focus;
        std::string s = "< " + b[i] + " >";
        text(bx, y, s, on ? AT_BARSEL : AT_BAR);
        put(bx + 2, y, (unsigned char)b[i][0], on ? AT_HOTSEL : AT_HOT);
        bx += (int)s.size() + 3;
    }
}

/* The button at column mx of a row drawn by draw_buttons, or -1. */
static int button_at(int x, int w, const std::vector<std::string> &b, int mx) {
    int bx = x + (w - buttons_width(b)) / 2;
    for (size_t i = 0; i < b.size(); i++) {
        int bw = (int)b[i].size() + 4;
        if (mx >= bx && mx < bx + bw) return (int)i;
        bx += bw + 3;
    }
    return -1;
}

static std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        size_t n = s.find('\n', a);
        v.push_back(s.substr(a, n == std::string::npos ? std::string::npos : n - a));
        if (n == std::string::npos) break;
        a = n + 1;
    }
    return v;
}

/* A message with buttons. Returns the button chosen, or -1 for Esc. */
static int message_box(const char *title, const std::string &msg,
                       const std::vector<std::string> &b, int def = 0) {
    std::vector<std::string> lines = split_lines(msg);
    int w = buttons_width(b) + 6;
    for (const std::string &l : lines) if ((int)l.size() + 6 > w) w = (int)l.size() + 6;
    int h = (int)lines.size() + 5;
    int focus = def;
    for (;;) {
        screen_size();
        draw_editor();
        int x, y;
        center_box(w, h, title, x, y);
        for (size_t i = 0; i < lines.size(); i++)
            text(x + (w - (int)lines[i].size()) / 2, y + 1 + (int)i, lines[i], AT_BAR);
        hsep(x, y + h - 3, w, AT_BAR);
        draw_buttons(x, y + h - 2, w, b, focus);
        flush();
        display_cursor(0);
        int k = getkey_m();
        if (k < 0) { quit_now = true; return -1; }
        if (IS_KEY_MOUSE(k)) {
            if ((m_press & 1) && m_y == y + h - 2) {
                int i = button_at(x, w, b, m_x);
                if (i >= 0) return i;
            }
            continue;
        }
        int base = k & ~KEY_SHIFT;
        if (k == 27) return -1;
        if (k == '\r' || k == ' ') return focus;
        if (k == '\t' || base == KEY_RIGHT || base == KEY_DOWN) focus = (focus + 1) % (int)b.size();
        else if (base == K_SHIFT_TAB || base == KEY_LEFT || base == KEY_UP)
            focus = (focus + (int)b.size() - 1) % (int)b.size();
        else if (k < 256 || alt_letter(k)) {
            int c = alt_letter(k) ? alt_letter(k) : toupper(k);
            for (size_t i = 0; i < b.size(); i++)
                if (toupper((unsigned char)b[i][0]) == c) return (int)i;
        }
    }
}

static void alert(const std::string &msg) { message_box("", msg, { "OK" }); }

/* A one-line text field: handles a key; returns false if it wasn't one. */
static bool field_key(std::string &s, int &pos, int k) {
    int base = k & ~KEY_SHIFT;
    if (base == KEY_LEFT)  { if (pos > 0) pos--; return true; }
    if (base == KEY_RIGHT) { if (pos < (int)s.size()) pos++; return true; }
    if (base == K_HOME)    { pos = 0; return true; }
    if (base == K_END)     { pos = (int)s.size(); return true; }
    if (k == 8)            { if (pos > 0) s.erase((size_t)--pos, 1); return true; }
    if (base == K_DEL)     { if (pos < (int)s.size()) s.erase((size_t)pos, 1); return true; }
    if (k == CTRL('Y'))    { s.clear(); pos = 0; return true; }
    if (k >= 32 && k < 256 && k != 127) { s.insert((size_t)pos++, 1, (char)k); return true; }
    return false;
}

/* Draw a field w wide at (x, y); returns the screen column of its cursor. */
static int draw_field(int x, int y, int w, const std::string &s, int pos, bool focus) {
    int off = pos >= w ? pos - w + 1 : 0;
    fill(x, y, w, 1, ' ', focus ? AT_FIELDF : AT_FIELD);
    std::string vis = s.substr((size_t)off < s.size() ? (size_t)off : s.size(), (size_t)w);
    text(x, y, vis, focus ? AT_FIELDF : AT_FIELD);
    return x + pos - off;
}

struct Field { const char *label; std::string *val; };

/* Text fields and buttons; Tab moves between them. Enter in a field is the
 * default button. Returns the button chosen or -1 for Esc. */
static int form(const char *title, const std::vector<Field> &fields,
                const std::vector<std::string> &b, int def = 0, const char *note = NULL) {
    int nf = (int)fields.size(), nb = (int)b.size();
    int lw = 0;
    for (const Field &f : fields) if ((int)strlen(f.label) > lw) lw = (int)strlen(f.label);
    int w = W - 8 < 64 ? W - 8 : 64;
    if (w < buttons_width(b) + 4) w = buttons_width(b) + 4;
    int extra = note ? 2 : 0;
    int h = nf * 2 + 4 + extra;
    std::vector<int> pos((size_t)nf);
    std::vector<char> fresh((size_t)nf, 1);   /* typing replaces a field's starting text */
    for (int i = 0; i < nf; i++) pos[(size_t)i] = (int)fields[(size_t)i].val->size();
    int focus = 0;   /* 0..nf-1 fields, then buttons */
    for (;;) {
        screen_size();
        draw_editor();
        int x, y;
        center_box(w, h, title, x, y);
        int cur_x = -1, cur_y = -1;
        if (note) text(x + 2, y + 1, std::string(note).substr(0, (size_t)(w - 4)), AT_BAR);
        for (int i = 0; i < nf; i++) {
            int fy = y + 2 + extra + i * 2 - 1;
            text(x + 2, fy, fields[(size_t)i].label, AT_BAR);
            int fx = x + 3 + lw, fw = w - lw - 5;
            int c = draw_field(fx, fy, fw, *fields[(size_t)i].val, pos[(size_t)i], focus == i);
            if (focus == i && fresh[(size_t)i] && !fields[(size_t)i].val->empty())
                fill(fx, fy, (int)fields[(size_t)i].val->size() < fw ? (int)fields[(size_t)i].val->size() : fw, 1,
                     ' ', AT_SEL), text(fx, fy, fields[(size_t)i].val->substr(0, (size_t)fw), AT_SEL);
            if (focus == i) { cur_x = c; cur_y = fy; }
        }
        hsep(x, y + h - 3, w, AT_BAR);
        draw_buttons(x, y + h - 2, w, b, focus >= nf ? focus - nf : -1);
        flush();
        show_cursor_at(cur_x, cur_y);
        int k = getkey_m();
        if (k < 0) { quit_now = true; return -1; }
        if (IS_KEY_MOUSE(k)) {
            if (!(m_press & 1)) continue;
            if (m_y == y + h - 2) {
                int i = button_at(x, w, b, m_x);
                if (i >= 0) return i;
            }
            for (int i = 0; i < nf; i++) {   /* a click in a field puts the cursor there */
                int fy = y + 2 + extra + i * 2 - 1, fx = x + 3 + lw, fw = w - lw - 5;
                if (m_y != fy || m_x < fx || m_x >= fx + fw) continue;
                int p = focus == i ? pos[(size_t)i] : (int)fields[(size_t)i].val->size();
                int off = p >= fw ? p - fw + 1 : 0;
                int np = off + m_x - fx, len = (int)fields[(size_t)i].val->size();
                pos[(size_t)i] = np < len ? np : len;
                focus = i;
                fresh[(size_t)i] = 0;
            }
            continue;
        }
        int base = k & ~KEY_SHIFT;
        if (k == 27) return -1;
        if (k == '\t' || (focus >= nf && (base == KEY_RIGHT || base == KEY_DOWN))) { focus = (focus + 1) % (nf + nb); continue; }
        if (base == K_SHIFT_TAB || (focus >= nf && (base == KEY_LEFT || base == KEY_UP))) { focus = (focus + nf + nb - 1) % (nf + nb); continue; }
        if (k == '\r') return focus >= nf ? focus - nf : def;
        if (focus < nf) {
            if (base == KEY_DOWN) { focus = (focus + 1) % (nf + nb); continue; }
            if (base == KEY_UP)   { focus = (focus + nf + nb - 1) % (nf + nb); continue; }
            if (fresh[(size_t)focus] && k >= 32 && k < 256 && k != 127) {
                fields[(size_t)focus].val->clear();
                pos[(size_t)focus] = 0;
            }
            fresh[(size_t)focus] = 0;
            field_key(*fields[(size_t)focus].val, pos[(size_t)focus], k);
        } else if (k == ' ') {
            return focus - nf;
        } else if (k < 256 || alt_letter(k)) {
            int c = alt_letter(k) ? alt_letter(k) : toupper(k);
            for (int i = 0; i < nb; i++)
                if (toupper((unsigned char)b[(size_t)i][0]) == c) return i;
        }
    }
}

/* A scrolling page of text (Help). */
static void viewer(const char *title, const std::string &body) {
    std::vector<std::string> lines = split_lines(body);
    int top = 0;
    for (;;) {
        screen_size();
        draw_editor();
        int w = W - 6 < 76 ? W - 6 : 76, h = H - 4;
        int x, y;
        center_box(w, h, title, x, y);
        int rows = h - 4;
        if (top > (int)lines.size() - rows) top = (int)lines.size() - rows;
        if (top < 0) top = 0;
        for (int i = 0; i < rows && top + i < (int)lines.size(); i++)
            text(x + 2, y + 1 + i, lines[(size_t)(top + i)].substr(0, (size_t)(w - 4)), AT_BAR);
        hsep(x, y + h - 3, w, AT_BAR);
        draw_buttons(x, y + h - 2, w, { "OK" }, 0);
        if ((int)lines.size() > rows) {
            char more[32];
            snprintf(more, sizeof more, " %d%% ", (int)((top + rows) * 100 / (int)lines.size()));
            text(x + w - (int)strlen(more) - 2, y + h - 3, more, AT_BAR);
        }
        flush();
        display_cursor(0);
        int k = getkey_m();
        if (k < 0) { quit_now = true; return; }
        if (IS_KEY_MOUSE(k)) {
            if (m_wheel) top -= 3 * m_wheel;
            else if ((m_press & 1) && m_y == y + h - 2 && button_at(x, w, { "OK" }, m_x) >= 0) return;
            continue;
        }
        int base = k & ~KEY_SHIFT;
        if (k == 27 || k == '\r' || k == ' ') return;
        if (base == KEY_DOWN) top++;
        else if (base == KEY_UP) top--;
        else if (base == K_PGDN) top += rows - 1;
        else if (base == K_PGUP) top -= rows - 1;
        else if (base == K_HOME || base == K_CTRL_HOME) top = 0;
        else if (base == K_END || base == K_CTRL_END) top = (int)lines.size();
    }
}

/* ================================================================
 * Files
 * ================================================================ */
static std::string expand_home(const std::string &p) {
    if (p.size() >= 1 && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        const char *home = getenv("HOME");
        if (home) return std::string(home) + p.substr(1);
    }
    return p;
}

static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    if (f) fclose(f);
    return f != NULL;
}

static std::string with_ext(const std::string &p) {
    std::string b = base_name(p);
    if (b.find('.') == std::string::npos && !file_exists(p)) return p + ".bas";
    return p;
}

static bool load_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        alert(std::string("File not found:\n") + path);
        return false;
    }
    std::string t;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
    fclose(f);
    set_text(t.data(), t.size());
    fname = path;
    modified = false;
    return true;
}

static bool write_file(const std::string &path) {
    capitalize_line(edited_line);
    std::string t = get_text(crlf ? "\r\n" : "\n");
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        alert(std::string("Can't write ") + path + "\n" + strerror(errno));
        return false;
    }
    bool ok = fwrite(t.data(), 1, t.size(), f) == t.size();
    if (fclose(f) != 0) ok = false;
    if (!ok) { alert(std::string("Error writing ") + path); return false; }
    fname = path;
    modified = false;
    snprintf(g_program_path, sizeof g_program_path, "%s", path.c_str());
    status_msg = "Saved " + path;
    return true;
}

static bool save_as(void) {
    std::string name = fname;
    for (;;) {
        char cwd[DEFAULT_BUFFER] = "";
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
        std::string note = std::string("Directory: ") + cwd;
        int r = form("Save As", { { "File Name:", &name } }, { "OK", "Cancel" }, 0, note.c_str());
        if (r != 0 || name.empty()) return false;
        std::string p = expand_home(name);
        if (is_dir(p)) {
            if (chdir(p.c_str()) == 0) name.clear();
            continue;
        }
        p = with_ext(p);
        if (p != fname && file_exists(p)) {
            int a = message_box("", base_name(p) + " already exists.\nOverwrite it?", { "Yes", "No" }, 1);
            if (a != 0) continue;
        }
        return write_file(p);
    }
}

static bool save(void) {
    if (fname.empty()) return save_as();
    return write_file(fname);
}

/* Offer to save changes. Returns false if the user cancelled. */
static bool keep_changes(void) {
    if (!modified) return true;
    int r = message_box("", "Save the changes made to the current program?", { "Yes", "No", "Cancel" });
    if (r == 0) return save();
    return r == 1;
}

struct DirEntry { std::string name; bool dir; };

static std::vector<DirEntry> list_dir(void) {
    std::vector<DirEntry> v;
    v.push_back(DirEntry{ "..", true });
    DIR *d = opendir(".");
    if (!d) return v;
    struct dirent *de;
    std::vector<DirEntry> dirs, files;
    while ((de = readdir(d))) {
        std::string n = de->d_name;
        if (n.empty() || n[0] == '.') continue;
        if (is_dir(n)) dirs.push_back(DirEntry{ n, true });
        else {
            size_t dot = n.rfind('.');
            if (dot != std::string::npos && strcasecmp(n.c_str() + dot, ".bas") == 0)
                files.push_back(DirEntry{ n, false });
        }
    }
    closedir(d);
    auto by_name = [](const DirEntry &a, const DirEntry &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
    /* insertion sort: no <algorithm> needed on every platform */
    auto sort_v = [&](std::vector<DirEntry> &x) {
        for (size_t i = 1; i < x.size(); i++)
            for (size_t j = i; j > 0 && by_name(x[j], x[j - 1]); j--) std::swap(x[j], x[j - 1]);
    };
    sort_v(dirs); sort_v(files);
    for (auto &e : dirs) v.push_back(e);
    for (auto &e : files) v.push_back(e);
    return v;
}

/* File > Open: a name field over a list of folders and .bas files. */
static void open_dialog(void) {
    if (!keep_changes()) return;
    std::string name;
    int pos = 0, focus = 0, sel_i = 0, scroll = 0;   /* focus: 0 field, 1 list, 2 OK, 3 Cancel */
    std::vector<DirEntry> ents = list_dir();
    for (;;) {
        screen_size();
        draw_editor();
        int w = W - 8 < 64 ? W - 8 : 64, h = H - 4 < 20 ? H - 4 : 20;
        int x, y;
        center_box(w, h, "Open", x, y);
        text(x + 2, y + 2, "File Name:", AT_BAR);
        int cur_x = draw_field(x + 13, y + 2, w - 16, name, pos, focus == 0);
        char cwd[DEFAULT_BUFFER] = "";
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
        std::string c = cwd;
        if ((int)c.size() > w - 4) c = "..." + c.substr(c.size() - (size_t)(w - 7));
        text(x + 2, y + 4, c, AT_BAR);
        int lx = x + 2, ly = y + 5, lw = w - 4, lh = h - 9;
        box(lx, ly, lw, lh + 2, AT_BAR);
        text(lx + 2, ly, " Files / Folders ", AT_BAR);
        if (sel_i >= (int)ents.size()) sel_i = (int)ents.size() - 1;
        if (sel_i < 0) sel_i = 0;
        if (sel_i < scroll) scroll = sel_i;
        if (sel_i >= scroll + lh) scroll = sel_i - lh + 1;
        for (int i = 0; i < lh && scroll + i < (int)ents.size(); i++) {
            const DirEntry &e = ents[(size_t)(scroll + i)];
            bool on = scroll + i == sel_i;
            unsigned char at = on ? (focus == 1 ? AT_BARSEL : A(15, 7)) : AT_BAR;
            std::string s = e.dir ? e.name + "/" : e.name;
            fill(lx + 1, ly + 1 + i, lw - 2, 1, ' ', at);
            text(lx + 2, ly + 1 + i, s.substr(0, (size_t)(lw - 4)), at);
        }
        hsep(x, y + h - 3, w, AT_BAR);
        draw_buttons(x, y + h - 2, w, { "OK", "Cancel" }, focus >= 2 ? focus - 2 : -1);
        flush();
        if (focus == 0) show_cursor_at(cur_x, y + 2); else display_cursor(0);

        int k = getkey_m();
        if (k < 0) { quit_now = true; return; }
        if (IS_KEY_MOUSE(k)) {
            /* the wheel goes through the list; a click picks a name (a
             * second click on it opens it); OK opens what's in the field */
            if (m_wheel) {
                sel_i -= 3 * m_wheel;
                if (sel_i >= (int)ents.size()) sel_i = (int)ents.size() - 1;
                if (sel_i < 0) sel_i = 0;
                if (focus != 1) focus = 1;
                if (!ents.empty() && !ents[(size_t)sel_i].dir) { name = ents[(size_t)sel_i].name; pos = (int)name.size(); }
                continue;
            }
            if (!(m_press & 1)) continue;
            if (m_y == y + h - 2) {
                int i = button_at(x, w, { "OK", "Cancel" }, m_x);
                if (i == 1) return;
                if (i != 0) continue;
                focus = 2;
                k = '\r';
            } else if (m_y == y + 2 && m_x >= x + 13 && m_x < x + 13 + w - 16) {
                focus = 0;
                int fw = w - 16, off = pos >= fw ? pos - fw + 1 : 0, np = off + m_x - (x + 13);
                pos = np < (int)name.size() ? np : (int)name.size();
                continue;
            } else if (m_y > ly && m_y <= ly + lh && m_x > lx && m_x < lx + lw - 1 &&
                       scroll + m_y - ly - 1 < (int)ents.size()) {
                int i = scroll + m_y - ly - 1;
                bool again = focus == 1 && i == sel_i;
                sel_i = i;
                focus = 1;
                if (!ents[(size_t)i].dir) { name = ents[(size_t)i].name; pos = (int)name.size(); }
                if (!again) continue;
                k = '\r';
            } else {
                continue;
            }
        }
        int base = k & ~KEY_SHIFT;
        if (k == 27 || (focus == 3 && (k == '\r' || k == ' '))) return;
        if (k == '\t') { focus = (focus + 1) % 4; continue; }
        if (base == K_SHIFT_TAB) { focus = (focus + 3) % 4; continue; }
        if (focus == 1 || (focus == 0 && (base == KEY_DOWN || base == KEY_UP || base == K_PGDN || base == K_PGUP))) {
            int old = sel_i;
            if (base == KEY_DOWN) sel_i++;
            else if (base == KEY_UP) sel_i--;
            else if (base == K_PGDN) sel_i += lh - 1;
            else if (base == K_PGUP) sel_i -= lh - 1;
            else if (base == K_HOME) sel_i = 0;
            else if (base == K_END) sel_i = (int)ents.size() - 1;
            if (sel_i >= (int)ents.size()) sel_i = (int)ents.size() - 1;
            if (sel_i < 0) sel_i = 0;
            if (sel_i != old || focus == 0) {
                if (focus == 0) focus = 1;
                if (!ents.empty() && !ents[(size_t)sel_i].dir) { name = ents[(size_t)sel_i].name; pos = (int)name.size(); }
                continue;
            }
        }
        if (k == '\r') {
            std::string target = name;
            if (focus == 1 && !ents.empty()) {
                const DirEntry &e = ents[(size_t)sel_i];
                target = e.name;
            }
            if (target.empty()) continue;
            std::string p = expand_home(target);
            if (is_dir(p)) {
                if (chdir(p.c_str()) == 0) { ents = list_dir(); sel_i = 0; scroll = 0; name.clear(); pos = 0; }
                continue;
            }
            p = with_ext(p);
            if (load_file(p)) { status_msg = "Loaded " + p; return; }
            continue;
        }
        if (focus == 0) field_key(name, pos, k);
        else if (focus == 1 && k > 32 && k < 256) {   /* jump to a name starting with the letter */
            for (size_t i = 1; i <= ents.size(); i++) {
                size_t j = ((size_t)sel_i + i) % ents.size();
                if (toupper((unsigned char)ents[j].name[0]) == toupper(k)) { sel_i = (int)j; break; }
            }
            if (!ents[(size_t)sel_i].dir) { name = ents[(size_t)sel_i].name; pos = (int)name.size(); }
        }
    }
}

/* ================================================================
 * Search
 * ================================================================ */
static size_t find_ci(const std::string &hay, const std::string &needle, size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); i++)
        if (strncasecmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return i;
    return std::string::npos;
}

/* Find the next match after the cursor, wrapping around; select it. */
static bool find_next(bool quiet = false) {
    if (find_str.empty()) return false;
    int n = (int)L.size();
    int startx = cx < len_of(cy) ? cx : len_of(cy);
    for (int i = 0; i <= n; i++) {
        int y = (cy + i) % n;
        size_t from = i == 0 ? (size_t)startx : 0;
        size_t p = find_ci(L[(size_t)y], find_str, from);
        if (i == n && p != std::string::npos && (int)p >= startx) p = std::string::npos;
        if (p != std::string::npos) {
            sel = true;
            ay = y; ax = (int)p;
            go_line(y, (int)(p + find_str.size()));
            return true;
        }
    }
    if (!quiet) alert("Match not found");
    return false;
}

static void find_dialog(void) {
    std::string s = find_str;
    if (!sel_empty()) { std::string t = sel_text(); if (t.find('\n') == std::string::npos) s = t; }
    if (form("Find", { { "Find What:", &s } }, { "OK", "Cancel" }) != 0 || s.empty()) return;
    find_str = s;
    sel = false;
    find_next();
}

static void change_dialog(void) {
    std::string f = find_str, c = change_str;
    if (!sel_empty()) { std::string t = sel_text(); if (t.find('\n') == std::string::npos) f = t; }
    int r = form("Change", { { "Find What:", &f }, { "Change To:", &c } },
                 { "Find and Verify", "Change All", "Cancel" });
    if (r < 0 || r == 2 || f.empty()) return;
    find_str = f; change_str = c;
    sel = false;
    int count = 0;
    bool snapped = false;
    auto change_at = [&](int y, size_t p) {
        if (!snapped) { snapshot(K_OTHER); snapped = true; }
        L[(size_t)y].replace(p, f.size(), c);
        count++;
        modified = true;
        edit_ver++;
    };
    if (r == 1) {
        /* Change All */
        for (int y = 0; y < (int)L.size(); y++) {
            size_t p = 0;
            while ((p = find_ci(L[(size_t)y], f, p)) != std::string::npos) {
                change_at(y, p);
                p += c.size();
            }
        }
    } else {
        /* Find and Verify: from the cursor to the end, then from the top */
        int n = (int)L.size();
        int sy = cy, sx = cx < len_of(cy) ? cx : len_of(cy);
        int y = sy;
        size_t x = (size_t)sx;
        bool wrapped = false;
        for (;;) {
            size_t p = find_ci(L[(size_t)y], f, x);
            if (wrapped && y == sy && p != std::string::npos && (int)p >= sx) p = std::string::npos;
            if (p == std::string::npos) {
                if (wrapped && y >= sy) break;
                if (++y >= n) { if (wrapped) break; wrapped = true; y = 0; }
                if (wrapped && y > sy) break;
                x = 0;
                continue;
            }
            sel = true; ay = y; ax = (int)p;
            go_line(y, (int)(p + f.size()));
            int a = message_box("Change", "Change this?", { "Yes", "Skip", "Cancel" });
            sel = false;
            if (a < 0 || a == 2) break;
            if (a == 0) {
                change_at(y, p);
                if (y == sy && (int)p < sx) sx += (int)c.size() - (int)f.size();
                x = p + c.size();
            } else {
                x = p + 1;
            }
        }
    }
    last_kind = 0;
    if (count) edited_line = -1;
    char m[64];
    snprintf(m, sizeof m, "%d change%s made", count, count == 1 ? "" : "s");
    if (count == 0 && r == 1) alert("Match not found"); else status_msg = m;
}

static void goto_dialog(void) {
    std::string s;
    if (form("Go to Line", { { "Line Number:", &s } }, { "OK", "Cancel" }) != 0) return;
    int n = atoi(s.c_str());
    if (n < 1) return;
    sel = false;
    go_line(n - 1, 0);
}

/* ================================================================
 * Running
 * ================================================================ */
static bool g_alt_screen;
static void terminal_screen(bool editor) {
#ifndef USE_SDL_WINDOW
    /* the terminal's alternate screen, with bracketed paste, while editing */
    if (editor && !g_alt_screen) display_print((char *)"\033[?1049h\033[?2004h");
    if (!editor && g_alt_screen) display_print((char *)"\033[?2004l\033[?1049l");
#endif
    g_alt_screen = editor;
}

/* Put the editor's text in memory as the program, as LOAD would. */
static bool sync_program(void) {
    capitalize_line(edited_line);
    std::string t = get_text();
    g_parse_error_active = 1;
    bool ok = true;
    if (setjmp(g_parse_error_jmp) == 0) {
        clear_program();
        load_text(t.data(), t.size());
        prescan_data();
    } else {
        ok = false;
    }
    g_parse_error_active = 0;
    snprintf(g_program_path, sizeof g_program_path, "%s", fname.c_str());
    return ok;
}

static void wait_any_key(void) {
    int rows = display_get_rows();
    display_locate(rows, 1);
    display_color(15, 0);
    display_print((char *)"Press any key to continue");
    display_raw_keys(1);
    int k;
    do k = getkey_m(); while (IS_KEY_MOUSE(k) && !m_press);   /* a click will do too */
    if (k < 0) quit_now = true;
}

static void run_program(bool cont) {
    if (cont && !can_continue()) cont = false;
    int start = 0;
    if (cont) {
        start = g_cont_pc;
    } else {
        if (!sync_program()) { alert("The program could not be loaded."); return; }
        if (g_nlines == 0) { status_msg = "There is no program to run"; return; }
        scope_program_start();
    }
    g_cont_pc = -1;

    /* the output screen */
#ifndef USE_SDL_WINDOW
    display_print((char *)"\033[?2004l");
#endif
    display_raw_keys(0);
    display_fill_screen(0);     /* programs get the usual 25 rows */
    display_color(7, 0);
    display_cls();
    display_cursor(1);

    bool parse_err = false;
    g_parse_error_active = 1;
    if (setjmp(g_parse_error_jmp) == 0) {
        run_from(start);
    } else {
        parse_err = true;
        g_cont_pc = -1;
    }
    g_parse_error_active = 0;
    int err_pc = parse_err ? g_current_pc : g_run_error_pc;
    run_ver = edit_ver;

    display_cursor(0);
    wait_any_key();

    /* back to the editor */
#ifdef USE_SDL_WINDOW
    if (gfx_active()) gfx_screen(0);
    display_fill_screen(1);
#else
    display_print((char *)"\033[?2004h");
#endif
    full_redraw = true;
    cur_at = -1;
    int at_pc = err_pc >= 0 ? err_pc : g_cont_pc;
    if (at_pc >= 0 && at_pc < g_nlines && g_lines[at_pc].srcline > 0 &&
        g_lines[at_pc].srcline <= (int)L.size()) {
        sel = false;
        go_line(g_lines[at_pc].srcline - 1, 0);
        sel = true; ay = cy; ax = 0; cx = len_of(cy);   /* highlight the line */
    }
    if (err_pc >= 0)
        status_msg = "The program stopped with an error at the highlighted line";
    else if (g_cont_pc >= 0)
        status_msg = "Break: F5 continues, Shift+F5 starts over";
}

/* ================================================================
 * Help
 * ================================================================ */
static const char *HELP_KEYS =
    "Menus\n"
    "  Alt or F10          Menu bar (then arrows, Enter, or a highlighted letter)\n"
    "  Alt+letter          Opens that menu, e.g. Alt+F for File\n"
    "  Esc                 Closes a menu or dialog box\n"
    "\n"
    "Running\n"
    "  F5                  Continue after a break, or run the program\n"
    "  Shift+F5            Start the program from the beginning\n"
    "  Ctrl+Break/Ctrl+C   Stop a running program\n"
    "  F6                  Go to the command line (EDIT comes back)\n"
    "\n"
    "Files\n"
    "  Ctrl+N  New         Ctrl+O  Open         Ctrl+S  Save\n"
    "\n"
    "Moving\n"
    "  Arrows, Home, End, PgUp, PgDn\n"
    "  Ctrl+Left/Right     Word left / right\n"
    "  Ctrl+Home/End       Top / bottom of the program\n"
    "  Ctrl+G              Go to a line number\n"
    "\n"
    "Editing\n"
    "  Shift+movement      Select text\n"
    "  Ctrl+A              Select all\n"
    "  Ctrl+X / Shift+Del  Cut\n"
    "  Ctrl+C / Ctrl+Ins   Copy\n"
    "  Ctrl+V / Shift+Ins  Paste\n"
    "  Ctrl+Y              Delete (cut) the current line\n"
    "  Ctrl+Z              Undo\n"
    "  Tab / Shift+Tab     Indent / outdent (the selected lines)\n"
    "  Ins                 Insert / overtype\n"
    "\n"
    "Search\n"
    "  Ctrl+F  Find        F3  Repeat last find        Ctrl+R  Change\n";

static const char *HELP_START =
    "Type your program in the window, with or without line numbers.\n"
    "Keywords are capitalized when you leave a line (Options menu).\n"
    "\n"
    "F5 runs it. When it ends, press any key to come back to the\n"
    "editor. If it stopped with an error, the line is highlighted.\n"
    "Press Ctrl+C (Ctrl+Break) to stop it; F5 then continues.\n"
    "\n"
    "The command line\n"
    "  File > Command Line (F6) takes you to the classic Ok prompt\n"
    "  with your program in memory: LIST, RUN, SAVE, numbered lines,\n"
    "  and statements typed to run at once all work there.\n"
    "  Type EDIT to come back to the editor.\n"
    "\n"
    "  Start Felix BASIC with --cli (or -c) to begin at the Ok prompt,\n"
    "  with --edit FILE (or -e FILE) to open a file in the editor, or\n"
    "  with just a file name to run it.\n";

static void about(void) {
    message_box("About", "Felix BASIC\n\nIn loving memory of Felix the lovebird.", { "OK" });
}

/* ================================================================
 * The editor
 * ================================================================ */
static void do_cmd(int cmd, int *result) {
    status_msg.clear();
    switch (cmd) {
    case CMD_NEW:
        if (!keep_changes()) break;
        set_text("", 0);
        fname.clear();
        modified = false;
        crlf = false;
        clear_program();
        break;
    case CMD_OPEN:    open_dialog(); break;
    case CMD_SAVE:    save(); break;
    case CMD_SAVEAS:  save_as(); break;
    case CMD_CLI:     *result = EDITOR_CLI; break;
    case CMD_EXIT:    if (keep_changes()) *result = EDITOR_EXIT; break;
    case CMD_UNDO:    undo(); break;
    case CMD_CUT:     if (!sel_empty()) { copy_sel(); delete_selection(); } break;
    case CMD_COPY:    copy_sel(); break;
    case CMD_PASTE:   paste(); break;
    case CMD_CLEAR:   if (!sel_empty()) delete_selection(); break;
    case CMD_DELLINE: cut_line(); break;
    case CMD_SELALL:
        sel = true; ay = 0; ax = 0;
        cy = (int)L.size() - 1; cx = len_of(cy);
        break;
    case CMD_FIND:     find_dialog(); break;
    case CMD_FINDNEXT: find_next(); break;
    case CMD_CHANGE:   change_dialog(); break;
    case CMD_GOTO:     goto_dialog(); break;
    case CMD_START:    run_program(false); break;
    case CMD_CONT:     run_program(true); break;
    case CMD_OPT_CAPS:   capitalize = !capitalize; break;
    case CMD_OPT_INDENT: auto_indent = !auto_indent; break;
    case CMD_OPT_OVR:    overwrite = !overwrite; break;
    case CMD_HELP_KEYS:  viewer("Keyboard", HELP_KEYS); break;
    case CMD_HELP_START: viewer("Getting Started", HELP_START); break;
    case CMD_HELP_ABOUT: about(); break;
    }
}

/* Bring in the program that's in memory, if it isn't what's being edited. */
static void take_program_from_memory(void) {
    size_t n = 0;
    const char *src = g_source_text(&n);
    std::string mem;
    bool from_listing = false;
    if (src) {
        mem.assign(src, n);
    } else if (g_nlines > 0) {
        /* typed at the command line: list it, as LIST does */
        int prev = -1;
        std::string line;
        for (int i = 0; i < g_nlines; i++) {
            if (g_lines[i].linenum < 0) continue;
            if (g_lines[i].linenum != prev) {
                if (prev != -1) mem += line + "\n";
                char num[16];
                snprintf(num, sizeof num, "%d ", g_lines[i].linenum);
                line = num;
                prev = g_lines[i].linenum;
            } else {
                line += ": ";
            }
            line += g_lines[i].text;
        }
        if (prev != -1) mem += line + "\n";
        from_listing = true;
    }
    std::string cur = get_text();
    std::string memn;   /* compare without CRs */
    for (char c : mem) if (c != '\r') memn += c;
    if (memn == cur || (memn.empty() && cur.empty())) {
        if (g_program_path[0]) fname = g_program_path;
        return;
    }
    set_text(mem.data(), mem.size());
    fname = g_program_path;
    modified = from_listing && !mem.empty();
}

/* ---- the mouse in the editing window ---- */
static bool m_dragging;      /* the left button went down in the text: dragging selects */

/* Scroll the text n lines, keeping the cursor in the window. */
static void scroll_view(int n) {
    top_line += n;
    if (top_line > (int)L.size() - 1) top_line = (int)L.size() - 1;
    if (top_line < 0) top_line = 0;
    if (cy < top_line) cy = top_line;
    if (cy > top_line + TH() - 1) cy = top_line + TH() - 1;
    clamp_cursor();
    sel = false;
}

static void editor_mouse(int *result) {
    int th = TH(), tw = TW();
    if (m_wheel) { scroll_view(-3 * m_wheel); return; }
    if (m_release & 1) { m_dragging = false; return; }
    if ((m_btn & 1) && !(m_press & 1)) {
        /* dragging: the selection runs from where the button went down */
        if (!m_dragging) return;
        int x = m_x - TX(), y = m_y - TY();
        if (y < 0) y = -1;                 /* past the edges: scrolls */
        if (y >= th) y = th;
        if (x < 0) x = left_col > 0 ? -1 : 0;
        if (x >= tw) x = tw;
        cx = left_col + x; cy = top_line + y;
        clamp_cursor();
        sel = cx != ax || cy != ay;
        return;
    }
    if (!(m_press & 1)) return;
    m_dragging = false;
    if (m_y == 0) {                        /* the menu bar */
        int m = menu_at(m_x);
        if (m >= 0) do_cmd(menu_loop(m, true), result);
        return;
    }
    if (m_y == H - 1) {                    /* the status line's keys */
        if (!status_keys_shown) return;
        static const int cmds[] = { CMD_HELP_KEYS, -1, CMD_CONT, CMD_CLI, CMD_SAVE };
        const char *p = STATUS_KEYS;
        int x = 1;
        for (int i = 0; i < 5 && *p; i++) {
            const char *e = strchr(p, '>');
            if (!e) break;
            int len = (int)(e - p) + 1;
            if (m_x >= x && m_x < x + len) {
                if (cmds[i] < 0) do_cmd(menu_loop(0, false), result);
                else do_cmd(cmds[i], result);
                return;
            }
            x += len + 1; p = e + 1;
            while (*p == ' ') p++;
        }
        return;
    }
    if (m_x == W - 1 && m_y >= TY() && m_y < TY() + th && th >= 3) {   /* the scroll bar */
        int track = th - 2, n = (int)L.size();
        int thumb = TY() + 1 + (n > 1 ? (int)((long long)cy * (track - 1) / (n - 1)) : 0);
        if (m_y == TY()) scroll_view(-1);
        else if (m_y == TY() + th - 1) scroll_view(1);
        else if (m_y < thumb) scroll_view(-(th - 1));
        else if (m_y > thumb) scroll_view(th - 1);
        return;
    }
    if (m_y == H - 2 && tw >= 3) {        /* the horizontal scroll bar's arrows */
        if (m_x == 1 && left_col > 0) { left_col--; if (cx >= left_col + tw) cx = left_col + tw - 1; }
        else if (m_x == W - 2 && left_col < 255) { left_col++; if (cx < left_col) cx = left_col; }
        return;
    }
    if (m_x >= TX() && m_x < TX() + tw && m_y >= TY() && m_y < TY() + th) {   /* the text */
        cx = left_col + m_x - TX();
        cy = top_line + m_y - TY();
        clamp_cursor();
        sel = false;
        ax = cx < len_of(cy) ? cx : len_of(cy); ay = cy;
        last_kind = 0;
        m_dragging = true;
    }
}

int editor_main(const char *file) {
    int result = -1;
    quit_now = false;
    screen_size();
    full_redraw = true;
    cur_at = -1;
    terminal_screen(true);
    display_raw_keys(1);
    display_fill_screen(1);

    static bool first = true;
    bool welcome = false;
    if (file && *file) {
        std::string p = with_ext(expand_home(file));
        if (file_exists(p)) load_file(p);
        else { set_text("", 0); fname = p; modified = false; status_msg = "New file " + p; }
    } else {
        take_program_from_memory();
        welcome = first && L.size() == 1 && L[0].empty();
    }
    first = false;

    if (welcome) {
        int r = message_box("", "Welcome to Felix BASIC\n\n"
                            "In loving memory of Felix the lovebird.\n\n"
                            "Press Enter to see the Survival Guide,\n"
                            "or Esc to clear this dialog box.", { "Survival Guide", "OK" });
        if (r == 0) viewer("Survival Guide", std::string(HELP_START) + "\n" + HELP_KEYS);
    }

    int prev_y = cy;
    while (result < 0 && !quit_now) {
        screen_size();
        clamp_cursor();
        scroll_to_cursor();
        draw_editor();
        flush();
        show_cursor_at(TX() + cx - left_col, TY() + cy - top_line);

        int k = getkey_m();
        if (k < 0) { quit_now = true; break; }
        if (k == 0) continue;   /* the screen changed size */
        if (IS_KEY_MOUSE(k)) {
            if (m_press || m_wheel) status_msg.clear();
            int keep_y = cy;
            editor_mouse(&result);
            if (cy != keep_y && edited_line >= 0 && edited_line != cy) {
                capitalize_line(edited_line);
                edited_line = -1;
            }
            continue;
        }
        if (k != KEY_PASTE_BEGIN && k != KEY_PASTE_END) status_msg.clear();

        int shift = k & KEY_SHIFT;
        int base = k & ~KEY_SHIFT;
        bool moved = true;
        int keep_y = cy;

        /* movement (Shift selects) */
        switch (base) {
        case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
        case K_HOME: case K_END: case K_PGUP: case K_PGDN:
        case K_CTRL_LEFT: case K_CTRL_RIGHT: case K_CTRL_HOME: case K_CTRL_END:
        case K_CTRL_PGUP: case K_CTRL_PGDN:
            if (shift && !sel) { sel = true; ax = cx < len_of(cy) ? cx : len_of(cy); ay = cy; }
            if (!shift) sel = false;
            last_kind = 0;
            switch (base) {
            case KEY_UP:    cy--; break;
            case KEY_DOWN:  cy++; break;
            case KEY_LEFT:
                if (cx > 0) cx--;
                else if (cy > 0 && shift) { cy--; cx = len_of(cy); }
                break;
            case KEY_RIGHT:
                if (shift && cx >= len_of(cy) && cy + 1 < (int)L.size()) { cy++; cx = 0; }
                else cx++;
                break;
            case K_HOME: {
                /* first non-blank, then column 1 */
                int nb = 0;
                while (nb < len_of(cy) && L[(size_t)cy][(size_t)nb] == ' ') nb++;
                cx = (cx == nb) ? 0 : nb;
                break;
            }
            case K_END:        cx = len_of(cy); break;
            case K_PGUP:       cy -= TH() - 1; top_line -= TH() - 1; if (top_line < 0) top_line = 0; break;
            case K_PGDN:       cy += TH() - 1; top_line += TH() - 1; break;
            case K_CTRL_PGUP:  cy = top_line; break;
            case K_CTRL_PGDN:  cy = top_line + TH() - 1; break;
            case K_CTRL_LEFT:  word_left(); break;
            case K_CTRL_RIGHT: word_right(); break;
            case K_CTRL_HOME:  cy = 0; cx = 0; break;
            case K_CTRL_END:   cy = (int)L.size() - 1; cx = len_of(cy); break;
            }
            if (top_line > (int)L.size() - 1) top_line = (int)L.size() - 1;
            if (top_line < 0) top_line = 0;
            clamp_cursor();
            break;
        default:
            moved = false;
        }

        if (!moved) {
            int al = alt_letter(base);
            if (k == KEY_PASTE_BEGIN) { pasting = true; continue; }
            if (k == KEY_PASTE_END)   { pasting = false; continue; }
            if (k == KEY_ALT_ALONE || base == K_F(10)) {
                do_cmd(menu_loop(0, false), &result);
            } else if (al) {
                int m = -1;
                for (int i = 0; i < NMENUS; i++) if (hot_char(MENUS[i].name) == al) m = i;
                if (m >= 0) do_cmd(menu_loop(m, true), &result);
            } else if (base == K_F(1)) {
                do_cmd(CMD_HELP_KEYS, &result);
            } else if (base == K_F(3)) {
                do_cmd(CMD_FINDNEXT, &result);
            } else if (base == K_F(5)) {
                do_cmd(shift ? CMD_START : CMD_CONT, &result);
            } else if (base == K_F(6)) {
                do_cmd(CMD_CLI, &result);
            } else if (k == CTRL('A')) { do_cmd(CMD_SELALL, &result);
            } else if (k == CTRL('C') || base == K_CTRL_INS) { copy_sel();
            } else if (k == CTRL('F')) { do_cmd(CMD_FIND, &result);
            } else if (k == CTRL('G')) { do_cmd(CMD_GOTO, &result);
            } else if (k == CTRL('N')) { do_cmd(CMD_NEW, &result);
            } else if (k == CTRL('O')) { do_cmd(CMD_OPEN, &result);
            } else if (k == CTRL('R')) { do_cmd(CMD_CHANGE, &result);
            } else if (k == CTRL('S')) { do_cmd(CMD_SAVE, &result);
            } else if (k == CTRL('V') || (base == K_INS && shift)) { paste();
            } else if (k == CTRL('X') || (base == K_DEL && shift)) { if (!sel_empty()) { copy_sel(); delete_selection(); }
            } else if (k == CTRL('Y')) { cut_line();
            } else if (k == CTRL('Z')) { undo();
            } else if (base == K_INS) { overwrite = !overwrite;
            } else if (base == K_DEL) { delete_char();
            } else if (k == 8) { backspace();
            } else if (k == '\t') { indent_lines(false);
            } else if (base == K_SHIFT_TAB) { indent_lines(true);
            } else if (k == '\r') { new_line();
            } else if (k == '\n') {
                /* LF: Enter, unless it's the LF of a pasted CR LF */
                if (!(pasting && last_key == '\r')) new_line();
            } else if (k == 27) {
                sel = false;
            } else if (k >= 32 && k < 256 && k != 127) {
                type_char(k);
            }
        }

        /* leaving a line typed on capitalizes its keywords */
        if (cy != keep_y && edited_line >= 0 && edited_line != cy) {
            capitalize_line(edited_line);
            edited_line = -1;
        }
        (void)prev_y;
        prev_y = cy;
        last_key = k;
    }

    if (quit_now) result = EDITOR_EXIT;
    if (result == EDITOR_CLI) sync_program();

    /* leave the screen as the command line expects it */
    display_raw_keys(0);
    display_fill_screen(0);
    terminal_screen(false);
    display_color(7, 0);
    display_cls();
    display_cursor(1);
    cur_at = -1;
    full_redraw = true;
    return result;
}

BASIC_NS_END

#endif /* BASIC_HAS_EDITOR */
