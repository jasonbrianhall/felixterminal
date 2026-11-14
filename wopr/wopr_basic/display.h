#ifndef DISPLAY_H
#define DISPLAY_H

#include "basic_ns.h"

BASIC_NS_BEGIN

/*
 * display.h — Terminal display abstraction for the BASIC interpreter
 *
 * All screen output goes through these functions.
 * To port to SDL or OpenGL, replace display.c with a new implementation
 * that satisfies this interface — basic.c never calls ANSI codes directly.
 */

/* Initialize / teardown */
void display_init(void);
void display_shutdown(void);

/* CLS — clear screen */
void display_cls(void);

/* LOCATE row, col  (1-based, as in BASIC) */
void display_locate(int row, int col);

/* COLOR fg, bg  (CGA colour indices 0-15) */
void display_color(int fg, int bg);

/* WIDTH cols — set terminal width (40 or 80) */
void display_width(int cols);

/* Key codes from the display layers beyond plain characters: the arrows
 * are 0x1000 up, 0x1001 down, 0x1002 left, 0x1003 right; other extended
 * keys are KEY_EXT(their PC scan code), e.g. KEY_EXT(59) for F1. INKEY$
 * gives CHR$(0) + CHR$(scan) for both. */
#define KEY_EXT(scan) (0x1100 | (scan))
#define SCAN_F1    59
#define SCAN_F10   68
#define SCAN_HOME  71
#define SCAN_PGUP  73
#define SCAN_END   79
#define SCAN_PGDN  81
#define SCAN_INS   82
#define SCAN_DEL   83
#define SCAN_F11  133
#define SCAN_F12  134
/* WIDTH , rows — text rows (25, 43, 50); call before display_width */
void display_text_rows(int rows);

/* Print a raw string (no newline) */
void display_print(char *s);

/* Print a single character */
void display_putchar(int c);

/* Newline */
void display_newline(void);

/* INKEY$ — non-blocking: returns 0 if no key waiting, else the char */
int display_inkey(void);

/* Blocking getchar (for single char reads) */
int display_getchar(void);

/* Blocking line read — reads until newline, stores in buf without the newline */
int display_getline(char *buf, int bufsz);

/* Hide / show cursor  (0 = hide, 1 = show) */
void display_cursor(int visible);

/* SPC(n) — print n spaces */
void display_spc(int n);

/* Get current terminal width (used by WIDTH query) */
int display_get_width(void);

/* The cursor's column, 1-based (POS, TAB and PRINT's comma zones). */
int display_get_col(void);

/* Terminal backends: note text written by other routes (basic_printf), so
 * display_get_col stays right. Escape sequences are understood. */
void display_note_output(const char *s);

/* ---- For the full-screen editor (editor.cpp) ----
 *
 * Rows of the text screen. */
int display_get_rows(void);

/* Editor key mode on/off. While on, display_getkey sees every key the
 * editor uses: Ctrl+letters as 1-26 (Ctrl+C and Ctrl+V too, not break or
 * paste), Alt+letter as KEY_EXT(its PC scan code) as INKEY$ gives it,
 * KEY_ALT_ALONE when Alt is pressed and let go by itself, and the
 * movement keys with KEY_SHIFT added when Shift is held (where the
 * keyboard can tell). A backend that can't tell some of these simply
 * doesn't send them. */
void display_raw_keys(int on);

/* Blocking key read for the editor: display_getchar's codes, plus the
 * extras above while editor key mode is on. Returns -1 if the window was
 * closed. */
int display_getkey(void);

/* Editor: use every text row the screen has room for. (The PC-style
 * displays otherwise keep 25 rows, which can leave bands above and below.) */
void display_fill_screen(int on);

/* The system clipboard where there is one: set copies text to it; get
 * returns a malloc'd copy of its text (free it), or NULL. */
void  display_clipboard_set(const char *text);
char *display_clipboard_get(void);

/* The mouse, for the editor: while editor key mode is on, display_getkey
 * also returns KEY_MOUSE(...) when a button is pressed or let go, for each
 * click of the wheel, and when the pointer moves to another text cell with
 * a button held. col and row count from 0; buttons: 1 left, 2 right,
 * 4 middle (those held now); wheel: +1 away from you, -1 towards you. */
#define KEY_MOUSE_FLAG 0x40000000
#define KEY_MOUSE(col, row, buttons, wheel) \
    (KEY_MOUSE_FLAG | ((col) & 0x1FF) | ((row) & 0xFF) << 9 | ((buttons) & 7) << 17 | \
     ((wheel) > 0 ? 1 : (wheel) < 0 ? 2 : 0) << 20)
#define IS_KEY_MOUSE(k)       ((k) >= 0 && ((k) & KEY_MOUSE_FLAG))
#define KEY_MOUSE_COL(k)      ((k) & 0x1FF)
#define KEY_MOUSE_ROW(k)      (((k) >> 9) & 0xFF)
#define KEY_MOUSE_BUTTONS(k)  (((k) >> 17) & 7)
#define KEY_MOUSE_WHEEL(k)    ((((k) >> 20) & 3) == 1 ? 1 : (((k) >> 20) & 3) == 2 ? -1 : 0)

#define KEY_SHIFT      0x10000
#define KEY_PASTE_BEGIN 0x20000   /* terminal bracketed paste: text follows */
#define KEY_PASTE_END   0x20001
#define KEY_ALT_ALONE  KEY_EXT(56)
#define KEY_UP         0x1000
#define KEY_DOWN       0x1001
#define KEY_LEFT       0x1002
#define KEY_RIGHT      0x1003

BASIC_NS_END

#endif /* DISPLAY_H */
