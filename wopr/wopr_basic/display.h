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

BASIC_NS_END

#endif /* DISPLAY_H */
