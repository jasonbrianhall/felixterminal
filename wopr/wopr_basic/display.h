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

BASIC_NS_END

#endif /* DISPLAY_H */
