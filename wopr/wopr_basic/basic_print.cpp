/* supp.c -- support routines for dungeon */
/* Modified for WOPR overlay */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <SDL2/SDL.h>
#include "basic_print.h"
#ifdef USE_SDL_WINDOW
#include "display.h"
#endif

#ifdef WOPR
#include "basic.h"
#include "../wopr.h"
BASIC_NS_BEGIN

void wopr_basic_text(WoprState *w, char *text);
void wopr_basic_push_line(char *text);
void wopr_basic_flush_partial(void);
void wopr_basic_cls(void);
void wopr_basic_color(int fg, int bg);
bool wopr_basic_is_waiting_input(WoprState *w);
void wopr_basic_color(int fg, int bg);
void wopr_basic_push_line(char *text);
BASIC_NS_END
#endif

#ifdef unix
#include <sys/types.h>
#endif

#ifdef BSD4_2
#include <sys/time.h>
#else
#include <time.h>
#endif
#include "basic_ns.h"

BASIC_NS_BEGIN

/* ============================================================================
 * Shared I/O surfaces
 * ========================================================================== */
#if defined(WOPR) || defined(FELIX_BASIC)

#ifdef FELIX_BASIC
/* Felix terminal host functions */
void felix_basic_push_line(char *line);
void felix_basic_flush_partial(void);
void felix_basic_cls(void);
void felix_basic_color(int fg);
/* Map the wopr_ names used in this file to felix_ equivalents */
static inline void wopr_basic_push_line(char *s) { felix_basic_push_line(s); }
static inline void wopr_basic_flush_partial(void)       { felix_basic_flush_partial(); }
#else
/* WOPR host functions */
void wopr_basic_push_line(char *line);
void wopr_basic_signal_done(void);
#endif

char          basic_input_buf[DEFAULT_BUFFER];
int           basic_input_ready         = 0;
int           g_basic_game_over         = 0;
int           g_basic_waiting_input     = 0;
int           g_basic_suppress_newline  = 0;
SDL_sem      *basic_input_sem           = NULL;

/* longjmp target set in basic_thread_fn before calling basic_main() */
jmp_buf       basic_exit_jmp;

void basic_shim_init(void)
{
    if (basic_input_sem) {
        SDL_DestroySemaphore(basic_input_sem);
        basic_input_sem = NULL;
    }
    basic_input_sem = SDL_CreateSemaphore(0);
}

void basic_shim_set_input(char *line)
{
    if (line && line[0] == '\0') {
        g_basic_game_over = 1;
        longjmp(basic_exit_jmp, 1);
        return;
    }

    strncpy(basic_input_buf, line, sizeof(basic_input_buf) - 1);
    basic_input_buf[sizeof(basic_input_buf) - 1] = '\0';
    basic_input_ready = 1;
    if (basic_input_sem) {
        SDL_SemPost(basic_input_sem);
    } else {
    }
}


char *basic_shim_fgets(char *buf, int n)
{
    // If BASIC is already shutting down, bail out via longjmp
    if (g_basic_game_over) {
        longjmp(basic_exit_jmp, 1);
    }

    // Prepare to wait for input
    wopr_basic_flush_partial();
    g_basic_waiting_input = 1;

    if (basic_input_sem) {
        SDL_SemWait(basic_input_sem);
    } else {
        while (!basic_input_ready && !g_basic_game_over) {
            SDL_Delay(10);
        }
    }

    g_basic_waiting_input = 0;

 
    // If BASIC was killed while waiting → bail out immediately
    if (g_basic_game_over) {
        longjmp(basic_exit_jmp, 1);
    }

    // No input was delivered (should be rare) — treat as EOF via longjmp
    if (!basic_input_ready) {
        longjmp(basic_exit_jmp, 1);
    }

    // CHR$(0) kill-switch (input path)
    if (basic_input_buf[0] == '\0') {
        g_basic_game_over = 1;
        longjmp(basic_exit_jmp, 1);
    }

    // Normal input path
    strncpy(buf, basic_input_buf, n - 1);
    buf[n - 1] = '\0';

    basic_input_ready = 0;
    g_basic_suppress_newline = 1;

    return buf;
}




/* ============================================================================
 * more_output
 * ========================================================================== */

static int  crows     = 24;
static int  coutput   = 0;
static char s_linebuf[4096];
static int  s_linelen = 0;

static void flush_linebuf(void)
{
    if (s_linelen > 0) {
        s_linebuf[s_linelen] = '\0';
        wopr_basic_push_line(s_linebuf);
        s_linelen = 0;
    }
}

void basic_more_init(void)
{
    crows     = 24;
    s_linelen = 0;
}

void basic_more_output(char *fmt, ...)
{
    char buf[4096];
    int  i;

    if (fmt == NULL) {
        flush_linebuf();
        coutput++;
        return;
    }

    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }

    for (i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '\n') {
            flush_linebuf();
        } else {
            if (s_linelen < (int)sizeof(s_linebuf) - 1)
                s_linebuf[s_linelen++] = buf[i];
        }
    }

    coutput++;
}

void basic_more_input(void)
{
    flush_linebuf();
    coutput = 0;
}

/* ============================================================================
 * Lifecycle
 * ========================================================================== */

void basic_exit_(void)
{
    flush_linebuf();
    g_basic_game_over = 1;
    /* Unblock any fgets waiting for input */
    if (basic_input_sem)
        SDL_SemPost(basic_input_sem);
    /* Jump all the way back to basic_thread_fn, unwinding the entire
     * dungeon call stack in one shot — no more dungeon code runs */
    longjmp(basic_exit_jmp, 1);
}

void basic_itime_(int *hrptr, int *minptr, int *secptr)
{
    time_t    timebuf;
    struct tm *tmptr;
    time(&timebuf);
    tmptr   = localtime(&timebuf);
    *hrptr  = tmptr->tm_hour;
    *minptr = tmptr->tm_min;
    *secptr = tmptr->tm_sec;
}

int basic_rnd_(int maxval)
{
    return rand() % maxval;
}
#endif /* WOPR || FELIX_BASIC */

int basic_printf(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#if defined(WOPR) || defined(FELIX_BASIC)
    display_note_output(buf);
    wopr_basic_push_line(buf);
#elif defined(USE_SDL_WINDOW)
    /* Route all output through the SDL text grid via display_print.
     * display.h is included transitively via basic.h / basic_print.h. */
    display_print(buf);
#else
    display_note_output(buf);
    fputs(buf, stdout);
    fflush(stdout);
#endif
    return r;
}


int basic_stderr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    int r = vfprintf(stderr, fmt, ap);

    va_end(ap);
    return r;
}

BASIC_NS_END
