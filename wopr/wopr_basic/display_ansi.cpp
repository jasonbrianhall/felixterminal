/*
 * display_ansi.c — ANSI/VT100 terminal implementation of display.h
 *
 * Three runtime backends selected by compile flags:
 *   (default)       — raw ANSI/VT100 to stdout
 *   -DWOPR          — WOPR overlay shim (wopr_basic_push_line etc.)
 *   -DFELIX_BASIC   — F8 standalone console shim (felix_basic_push_line etc.)
 *
 * CGA colour mapping to ANSI:
 *   0=Black 1=Blue 2=Green 3=Cyan 4=Red 5=Magenta 6=Brown/Yellow 7=LightGrey
 *   8=DarkGrey 9=LightBlue 10=LightGreen 11=LightCyan 12=LightRed
 *   13=LightMagenta 14=Yellow 15=White
 */

#include "display.h"
#include "sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#ifdef WOPR
#include <SDL2/SDL.h>
#elif defined(FELIX_BASIC)
#include <SDL2/SDL.h>
#include "felix_basic.h"
#elif defined(_WIN32)
#include <SDL2/SDL.h>
#endif

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#endif

#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

// Forward declaration of wopr_basic_color from wopr_basic.cpp
#ifdef WOPR
extern void wopr_basic_color(int fg, int bg);
#endif

BASIC_NS_BEGIN

/* ----------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------- */
#ifndef _WIN32
static struct termios g_orig_termios;
static int g_raw = 0;
#endif
static int g_width = 80;

/* CGA index → ANSI colour number (for fg: 30+n, bg: 40+n) */
static const int cga_to_ansi[16] = {
    0, 4, 2, 6, 1, 5, 3, 7,   /* 0-7  normal */
    0, 4, 2, 6, 1, 5, 3, 7    /* 8-15 bright (handled via bold) */
};

/* ----------------------------------------------------------------
 * Raw mode helpers
 * ---------------------------------------------------------------- */
#ifndef _WIN32
static void enter_raw(void)
{
    if (g_raw) return;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    g_raw = 1;
}

static void leave_raw(void)
{
    if (!g_raw) return;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    g_raw = 0;
}
#else
static void enter_raw(void) {}
static void leave_raw(void) {}
#endif

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

static void cleanup_terminal(void) {
    leave_raw();
    printf("\033[0m\033[?25h\n");
    fflush(stdout);
}

static void signal_handler(int sig) {
    cleanup_terminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

void display_init(void)
{
#ifndef _WIN32
    tcgetattr(STDIN_FILENO, &g_orig_termios);
#endif
#if !defined(WOPR) && !defined(FELIX_BASIC)
    enter_raw();
#endif
    atexit(cleanup_terminal);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
#if !defined(WOPR) && !defined(FELIX_BASIC)
#  ifndef _WIN32
    printf("\033[0m\033[2J\033[H");
    fflush(stdout);
#  endif
#endif
}

void display_shutdown(void)
{
#if !defined(WOPR) && !defined(FELIX_BASIC)
    sound_shutdown();
#endif
}

void display_cls(void)
{
#if defined(FELIX_BASIC)
    felix_basic_cls();
#else
    printf("\033[2J\033[H");
    fflush(stdout);
#endif
}

void display_locate(int row, int col)
{
#if defined(FELIX_BASIC)
    (void)row; (void)col;
    felix_basic_flush_partial();
#else
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    printf("\033[%d;%dH", row, col);
    fflush(stdout);
#endif
}

void display_color(int fg, int bg)
{
#if defined(FELIX_BASIC)
    (void)bg;
    felix_basic_color(fg);
#endif
#if defined(WOPR)
    // Call wopr_basic_color which handles CGA→RGB conversion and updates globals
    wopr_basic_color(fg, bg);
#else
    if (fg < 0 || fg > 15) fg = 7;
    if (bg < 0 || bg > 15) bg = 0;

    int bold   = (fg >= 8) ? 1 : 0;
    int fg_idx = cga_to_ansi[fg & 7];
    int bg_idx = cga_to_ansi[bg & 7];

    printf("\033[%d;%d;%dm", bold, 30 + fg_idx, 40 + bg_idx);
    fflush(stdout);
#endif
}

void display_width(int cols)
{
#if defined(FELIX_BASIC)
    (void)cols;
#else
    g_width = cols;
    if (cols == 40)
        printf("\033[?3h");
    else
        printf("\033[?3l");
    fflush(stdout);
#endif
}

void display_print(char *s)
{
#ifdef WOPR
    if (strcmp(s, "Ok\n") == 0) return;
    // Suppress output while waiting for input to avoid double echo
    if (g_basic_waiting_input) return;
    g_basic_suppress_newline = 0;
    wopr_basic_push_line(s);
#elif defined(FELIX_BASIC)
    felix_basic_push_line(s);
#else
    fputs(s, stdout);
    fflush(stdout);
#endif
}

void display_putchar(int c)
{
#ifdef WOPR
    g_basic_suppress_newline = 0;
    char tmp[2] = { (char)c, 0 };
    wopr_basic_push_line(tmp);
#elif defined(FELIX_BASIC)
    char tmp[2] = { (char)c, 0 };
    felix_basic_push_line(tmp);
#else
    putchar(c);
    fflush(stdout);
#endif
}

void display_newline(void)
{
#ifdef WOPR
    if (g_basic_suppress_newline) {
        g_basic_suppress_newline = 0;
        return;
    }
    wopr_basic_push_line("\n");
#elif defined(FELIX_BASIC)
    felix_basic_push_line("\n");
#else
    putchar('\n');
    fflush(stdout);
#endif
}

void display_cursor(int visible)
{
#if defined(FELIX_BASIC)
    (void)visible;
#else
    if (visible)
        printf("\033[?25h");
    else
        printf("\033[?25l");
    fflush(stdout);
#endif
}

void display_spc(int n)
{
#ifdef WOPR
    for (int i = 0; i < n; i++) wopr_basic_push_line(" ");
#elif defined(FELIX_BASIC)
    for (int i = 0; i < n; i++) felix_basic_push_line(" ");
#else
    for (int i = 0; i < n; i++) putchar(' ');
    fflush(stdout);
#endif
}

int display_get_width(void)
{
#if defined(WOPR) || defined(FELIX_BASIC)
    return g_width;
#elif !defined(_WIN32)
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return g_width;
#else
    return g_width;
#endif
}

/* INKEY$ — non-blocking */
int display_inkey(void)
{
#ifdef WOPR
    // Flush any pending partial line so it shows as the prompt while
    // the program spins on INKEY$ waiting for a keypress.
    wopr_basic_flush_partial();
    if (g_basic_game_over) longjmp(basic_exit_jmp, 1);
    int c = wopr_basic_get_key();
    if (c >= 0) return c;
#  ifdef _WIN32
    SDL_Delay(1);
#  else
    usleep(1000);
#  endif
    return 0;
#elif defined(FELIX_BASIC)
    int c = felix_basic_get_key();
    if (c >= 0) return c;
    SDL_Delay(1);
    return 0;
#elif !defined(_WIN32)
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return (int)c;
    usleep(1000);
    return 0;
#else
    SDL_Delay(1);
    return 0;
#endif
}

/* Blocking line read */
int display_getline(char *buf, int bufsz)
{
#ifdef WOPR
    if (!basic_shim_fgets(buf, bufsz)) {
        buf[0] = '\0';
        return 0;
    }
    // Don't echo here - wopr_basic_keydown already handled echo when Enter was pressed
    g_basic_suppress_newline = 1;
    return (int)strlen(buf);
#elif defined(FELIX_BASIC)
    felix_basic_shim_fgets(buf, bufsz);
    return (int)strlen(buf);
#elif !defined(_WIN32)
{
    /* Raw-mode line editor with up/down history */
    #define HIST_MAX 64

    static char (*s_hist)[512] = nullptr;
    static int   s_hist_count = 0;
    static int   s_hist_head  = 0;

    struct HistInit {
        HistInit() {
            s_hist = new char[HIST_MAX][512];
        }
        ~HistInit() {
            delete[] s_hist;
        }
    };

    static HistInit _hist_init;


    {
        /* We want blocking reads, but still raw (no echo, no canonical) */
        struct termios raw = g_orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_raw = 1;
    }

    char tmp[512];
    int  len    = 0;
    int  cursor = 0;
    int  hist_pos = s_hist_count;   /* points past end = "new line" */

    /* saved copy of what the user was typing before scrolling history */
    char saved[512] = "";

    /* Query the current cursor column so redraw can return to it after \r,
     * preserving whatever prompt was already printed on this line.
     * We send ESC[6n and read back ESC[row;colR. Fall back to col 0 on failure. */
    int prompt_col = 0;
    {
        /* Switch to blocking for the query response */
        struct termios tmp_t = g_orig_termios;
        tmp_t.c_lflag &= ~(ECHO | ICANON);
        tmp_t.c_cc[VMIN]  = 1;
        tmp_t.c_cc[VTIME] = 1;
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);
        tcsetattr(STDIN_FILENO, TCSANOW, &tmp_t);

        write(STDOUT_FILENO, "\033[6n", 4);
        char rbuf[32]; int ri = 0;
        /* read ESC [ rows ; cols R */
        unsigned char ch;
        while (ri < (int)sizeof(rbuf) - 1) {
            if (read(STDIN_FILENO, &ch, 1) <= 0) break;
            rbuf[ri++] = (char)ch;
            if (ch == 'R') break;
        }
        rbuf[ri] = '\0';
        /* parse \033[row;colR */
        int row = 0, col = 0;
        if (sscanf(rbuf, "\033[%d;%dR", &row, &col) == 2 && col > 1)
            prompt_col = col - 1;   /* 1-based → 0-based offset */
    }

    auto redraw = [&]() {
        /* Return to start of line, skip over the prompt, rewrite input, clear tail */
        write(STDOUT_FILENO, "\r", 1);
        if (prompt_col > 0) {
            char mv[16];
            snprintf(mv, sizeof mv, "\033[%dC", prompt_col);
            write(STDOUT_FILENO, mv, strlen(mv));
        }
        write(STDOUT_FILENO, tmp, len);
        write(STDOUT_FILENO, "\033[K", 3);   /* erase to end of line */
        /* reposition cursor within input */
        if (cursor < len) {
            char mv[16];
            int back = len - cursor;
            snprintf(mv, sizeof mv, "\033[%dD", back);
            write(STDOUT_FILENO, mv, strlen(mv));
        }
    };

    int interrupted = 0;
    for (;;) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            if (n < 0 && errno == EINTR && g_break) interrupted = 1;   /* Ctrl+C */
            break;
        }

        if (c == 3) { interrupted = 1; break; }   /* Ctrl+C, when the terminal passes it on */
        if (c == '\n' || c == '\r') {
            write(STDOUT_FILENO, "\r\n", 2);
            break;

        } else if (c == 127 || c == 8) {   /* Backspace / DEL */
            if (cursor > 0) {
                memmove(tmp + cursor - 1, tmp + cursor, len - cursor);
                cursor--; len--;
                redraw();
            }

        } else if (c == 1) {   /* Ctrl-A — home */
            cursor = 0;
            redraw();

        } else if (c == 5) {   /* Ctrl-E — end */
            cursor = len;
            redraw();

        } else if (c == 11) {  /* Ctrl-K — kill to end */
            len = cursor;
            redraw();

        } else if (c == 27) {  /* ESC — expect [ then code */
            unsigned char seq[4] = {0};
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (seq[0] != '[') continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;

            if (seq[1] == 'A') {   /* Up arrow — older history */
                if (s_hist_count == 0) continue;
                if (hist_pos == s_hist_count) {
                    /* save current edit */
                    tmp[len] = '\0';
                    strncpy(saved, tmp, sizeof saved - 1);
                }
                if (hist_pos > 0) hist_pos--;
                int idx = (s_hist_head - s_hist_count + hist_pos + HIST_MAX) % HIST_MAX;
                strncpy(tmp, s_hist[idx], sizeof tmp - 1);
                len = cursor = (int)strlen(tmp);
                redraw();

            } else if (seq[1] == 'B') {   /* Down arrow — newer history */
                if (hist_pos < s_hist_count) hist_pos++;
                if (hist_pos == s_hist_count) {
                    strncpy(tmp, saved, sizeof tmp - 1);
                } else {
                    int idx = (s_hist_head - s_hist_count + hist_pos + HIST_MAX) % HIST_MAX;
                    strncpy(tmp, s_hist[idx], sizeof tmp - 1);
                }
                len = cursor = (int)strlen(tmp);
                redraw();

            } else if (seq[1] == 'C') {   /* Right arrow */
                if (cursor < len) {
                    cursor++;
                    write(STDOUT_FILENO, "\033[C", 3);
                }

            } else if (seq[1] == 'D') {   /* Left arrow */
                if (cursor > 0) {
                    cursor--;
                    write(STDOUT_FILENO, "\033[D", 3);
                }

            } else if (seq[1] == '3') {   /* Delete key: ESC [ 3 ~ */
                read(STDIN_FILENO, &seq[2], 1);   /* consume ~ */
                if (cursor < len) {
                    memmove(tmp + cursor, tmp + cursor + 1, len - cursor - 1);
                    len--;
                    redraw();
                }
            }

        } else if (c >= 32 && c < 127) {   /* printable */
            if (len < bufsz - 1 && len < (int)sizeof(tmp) - 1) {
                memmove(tmp + cursor + 1, tmp + cursor, len - cursor);
                tmp[cursor++] = (char)c;
                len++;
                redraw();
            }
        }
    }

    tmp[len] = '\0';

    /* Add to history if non-empty and different from last entry */
    if (len > 0) {
        int last = (s_hist_head - 1 + HIST_MAX) % HIST_MAX;
        if (s_hist_count == 0 || strcmp(s_hist[last], tmp) != 0) {
            strncpy(s_hist[s_hist_head], tmp, sizeof s_hist[0] - 1);
            s_hist_head = (s_hist_head + 1) % HIST_MAX;
            if (s_hist_count < HIST_MAX) s_hist_count++;
        }
    }

    strncpy(buf, tmp, bufsz - 1);
    buf[bufsz - 1] = '\0';
    /* Restore nonblocking raw mode for INKEY$ etc.
     * We can't use enter_raw() here because g_raw is already 1 (we
     * switched to blocking inside the line editor without clearing it),
     * so enter_raw() would early-return and stdin would stay blocking. */
    {
        struct termios raw = g_orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        g_raw = 1;
    }
    if (interrupted) {             /* Ctrl+C at the prompt closes BASIC */
        g_break = 0;
        write(STDOUT_FILENO, "\r\n", 2);
        buf[0] = '\0';
        return -1;
    }
    return len;
}
#else
    if (!fgets(buf, bufsz, stdin)) { buf[0] = '\0'; return 0; }
    int len = (int)strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return len;
#endif
}

/* Single blocking getchar */
int display_getchar(void)
{
#ifdef WOPR
    char buf[2] = {0};
    if (!basic_shim_fgets(buf, sizeof(buf))) return 0;
    return (unsigned char)buf[0];
#elif defined(FELIX_BASIC)
    char buf[2] = {0};
    felix_basic_shim_fgets(buf, sizeof(buf));
    return (unsigned char)buf[0];
#elif !defined(_WIN32)
    leave_raw();

    struct termios cooked = g_orig_termios;
    cooked.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);

    char c = 0;
    ssize_t nr = read(STDIN_FILENO, &c, 1);
    (void)nr;

    enter_raw();
    return (unsigned char)c;
#else
    int c = getchar();
    return (c == EOF) ? 0 : (unsigned char)c;
#endif
}

BASIC_NS_END
