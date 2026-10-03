/*
 * commands.c All BASIC command handlers, command registration table,
 *              statement splitter, and dispatcher.
 */
#include <stdint.h>
#include "basic.h"
#include <stdarg.h>
#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef USE_SDL_WINDOW
/* Forward declarations  defined at global scope in basic_gfx_sdl.cpp. */
bool gfx_sdl_pump(void);
void gfx_sdl_render(void);
#endif

/* gfx_screen_ex lives in global scope (like all gfx_* functions). */
#ifdef USE_SDL_WINDOW
void gfx_screen_ex(int mode, int colorswitch, int apage, int vpage);
#endif

BASIC_NS_BEGIN

/* Current Graphics Position  updated by PSET, PRESET, LINE endpoint */
static double g_gfx_x = 0.0, g_gfx_y = 0.0;

/* Resolve a color value from _RGB() encoding (0x01RRGGBB) to a raw 0x00RRGGBB
 * int that gfx_* functions accept. In truecolor mode gfx_* uses the full value;
 * in palette mode gfx_* takes only the low 4 bits (palette index). */
static int color_resolve(long packed) {
    if ((packed & 0xFF000000L) == 0x01000000L)
        return (int)(packed & 0x00FFFFFFu);  /* strip our flag byte  0x00RRGGBB */
    return (int)(packed & 0x00FFFFFFu);      /* already a plain value */
}


/* Expand a leading ~/ or ~ to $HOME in-place */
static void tilde_expand(char *buf, int bufsz) {
    if (buf[0] != '~') return;
    if (buf[1] != '/' && buf[1] != '\0') return;
    const char *home = getenv("HOME");
    if (!home) return;
    char tmp[DEFAULT_BUFFER];
    snprintf(tmp, sizeof(tmp), "%s%s", home, buf + 1);
    strncpy(buf, tmp, bufsz - 1);
    buf[bufsz - 1] = '\0';
}

/* ================================================================
 * Stack trace helper  prints current line and GOSUB call chain,
 * then exits.  Called from stack overflow and other fatal errors.
 * ================================================================ */
static void basic_stacktrace(const char *reason) {
    basic_stderr("\n%s\n", reason);
    if (g_current_pc >= 0 && g_current_pc < g_nlines)
        basic_stderr("  at line %d: %s\n",
                     g_lines[g_current_pc].linenum,
                     g_lines[g_current_pc].text);
    basic_stderr("BASIC call stack (most recent first):\n");
    int shown = 0;
    for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
        if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0) {
            int ret_pc = g_ctrl[fi].line_idx - 1;
            if (ret_pc >= 0 && ret_pc < g_nlines)
                basic_stderr("  called from line %d: %s\n",
                             g_lines[ret_pc].linenum,
                             g_lines[ret_pc].text);
            else
                basic_stderr("  called from line <unknown>\n");
            shown++;
        } else if (strcmp(g_ctrl[fi].varname, "\x03" "WHILE") == 0) {
            int wpc = g_ctrl[fi].line_idx;
            if (wpc >= 0 && wpc < g_nlines)
                basic_stderr("  inside WHILE at line %d: %s\n",
                             g_lines[wpc].linenum, g_lines[wpc].text);
            shown++;
        } else if (strcmp(g_ctrl[fi].varname, "\x02" "DO") == 0) {
            int dpc = g_ctrl[fi].line_idx;
            if (dpc >= 0 && dpc < g_nlines)
                basic_stderr("  inside DO at line %d: %s\n",
                             g_lines[dpc].linenum, g_lines[dpc].text);
            shown++;
        } else if (g_ctrl[fi].varname[0] != '\x01' &&
                   g_ctrl[fi].varname[0] != '\x02' &&
                   g_ctrl[fi].varname[0] != '\x03') {
            /* FOR loop frame */
            basic_stderr("  inside FOR %s loop\n", g_ctrl[fi].varname);
            shown++;
        }
        if (shown >= 32) { basic_stderr("  ... (truncated)\n"); break; }
    }
}

/* ================================================================
 * PRINT USING -- QBasic's format fields:
 *   numbers  # digit, . point, , thousands (left of the point),
 *            leading + or trailing + / - for the sign, $$ ** **$ fills,
 *            ^^^^ exponent
 *   strings  ! first character, & all of it, \  \ that many characters
 *   _x       a literal x; anything else is printed as it stands
 * ================================================================ */
static int using_is_num_start(const char *f) {
    if (*f == '#') return 1;
    if (*f == '.' && f[1] == '#') return 1;
    if (*f == '+' && (f[1] == '#' || (f[1] == '.' && f[2] == '#') ||
                      (f[1] == '$' && f[2] == '$') || (f[1] == '*' && f[2] == '*'))) return 1;
    if (*f == '$' && f[1] == '$') return 1;
    if (*f == '*' && f[1] == '*') return 1;
    return 0;
}
static int using_is_str_start(const char *f) {
    if (*f == '!' || *f == '&') return 1;
    if (*f == '\\') {
        for (const char *q = f + 1; *q; q++) { if (*q == '\\') return 1; if (*q != ' ') return 0; }
    }
    return 0;
}

/* Format one number by the numeric field at f; returns the field's length. */
static int using_number(const char *f, double val, char *out, int outsz) {
    const char *q = f;
    int lead_plus = 0, trail_plus = 0, trail_minus = 0, dollar = 0, stars = 0;
    int before = 0, after = 0, dot = 0, commas = 0, expo = 0;
    if (*q == '+') { lead_plus = 1; q++; }
    if (q[0] == '*' && q[1] == '*') { stars = 1; before += 2; q += 2; if (*q == '$') { dollar = 1; q++; } }
    else if (q[0] == '$' && q[1] == '$') { dollar = 1; before += 1; q += 2; }
    for (;;) {
        if (*q == '#') { if (dot) after++; else before++; q++; }
        else if (*q == ',' && !dot) { commas = 1; before++; q++; }
        else if (*q == '.' && !dot) { dot = 1; q++; }
        else break;
    }
    if (q[0] == '^' && q[1] == '^' && q[2] == '^' && q[3] == '^') { expo = 1; q += 4; if (*q == '^') q++; }
    if (!lead_plus) {
        if (*q == '+') { trail_plus = 1; q++; }
        else if (*q == '-') { trail_minus = 1; q++; }
    }
    int neg = val < 0;
    double a = neg ? -val : val;
    char digits[400];
    if (expo) {
        int e = 0;
        if (a != 0) { e = (int)floor(log10(a)) - (before - 1); a /= pow(10.0, e); }
        if (before < 1) { before = 1; }
        snprintf(digits, sizeof digits, "%.*fE%+03d", after, a, e);
    } else {
        snprintf(digits, sizeof digits, "%.*f", after, a);
        if (commas) {   /* group the integer part */
            char *pt = strchr(digits, '.');
            int ilen = pt ? (int)(pt - digits) : (int)strlen(digits);
            char g[400]; int gi = 0;
            for (int k = 0; k < ilen; k++) {
                g[gi++] = digits[k];
                int left = ilen - 1 - k;
                if (left > 0 && left % 3 == 0) g[gi++] = ',';
            }
            snprintf(g + gi, sizeof g - gi, "%s", pt ? pt : "");
            strcpy(digits, g);
        }
        /* QBasic drops a lone leading 0 before the point when there's no
         * room for it: .## shows .50 */
        if (dot && before == 0 && digits[0] == '0' && digits[1] == '.') memmove(digits, digits + 1, strlen(digits));
    }
    char body[440];
    const char *sign = "";
    if (lead_plus) sign = neg ? "-" : "+";
    else if (neg && !trail_minus && !trail_plus) sign = "-";
    snprintf(body, sizeof body, "%s%s%s", sign, dollar ? "$" : "", digits);
    int width = before + (dot ? 1 + after : 0) + (lead_plus ? 1 : 0) + (dollar && stars ? 1 : 0) + (expo ? 4 : 0);
    int blen = (int)strlen(body);
    int o = 0;
    if (blen > width && o < outsz - 1) out[o++] = '%';       /* doesn't fit */
    for (int k = blen; k < width && o < outsz - 1; k++) out[o++] = stars ? '*' : ' ';
    for (int k = 0; k < blen && o < outsz - 1; k++) out[o++] = body[k];
    if (trail_plus && o < outsz - 1) out[o++] = neg ? '-' : '+';
    if (trail_minus && o < outsz - 1) out[o++] = neg ? '-' : ' ';
    out[o] = '\0';
    return (int)(q - f);
}

/* PRINT USING fmt$; values -- the fields take the values in turn, the
 * format starting over if there are more values than fields. */
static char *print_using_list(char *fmt, char *p) {
    char out[2048]; int o = 0;
    int pos = 0, used_field = 0, flen = (int)strlen(fmt);
    int trailing = 0;
    #define EMIT(c) do { if (o < (int)sizeof out - 1) out[o++] = (c); } while (0)
    for (;;) {
        /* copy literals up to the next field (or the end of the format) */
        while (pos < flen && !using_is_num_start(fmt + pos) && !using_is_str_start(fmt + pos)) {
            if (fmt[pos] == '_' && pos + 1 < flen) { EMIT(fmt[pos + 1]); pos += 2; }
            else EMIT(fmt[pos++]);
        }
        if (pos >= flen) {
            if (!used_field || !*p || trailing == 0) break;   /* no field at all, or no more values */
            pos = 0; continue;                                 /* reuse the format */
        }
        if (!*p || *p == ':' || *p == '\'') break;             /* out of values: stop at the field */
        used_field = 1;
        const char *f = fmt + pos;
        if (using_is_str_start(f)) {
            char sbuf[1024];
            p = sk(eval_str_expr(p, sbuf, sizeof sbuf));
            int n = (*f == '!') ? 1 : (*f == '&') ? -1 : 0;
            int w = 1;
            if (*f == '\\') { const char *q = f + 1; while (*q != '\\') q++; n = (int)(q - f) + 1; w = n; }
            if (n < 0) { for (char *c = sbuf; *c; c++) EMIT(*c); }
            else {
                int sl = (int)strlen(sbuf);
                for (int k = 0; k < n; k++) EMIT(k < sl ? sbuf[k] : ' ');
            }
            pos += (*f == '\\') ? w : 1;
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = sk(eval_expr(p, val));
            char nb[512];
            pos += using_number(f, mpf_get_d(val), nb, sizeof nb);
            mpf_clear(val);
            for (char *c = nb; *c; c++) EMIT(*c);
        }
        if (*p == ';' || *p == ',') { p = sk(p + 1); trailing = 1; }
        else trailing = 0;
        if (!*p || *p == ':' || *p == '\'') {
            /* values done: print the literals up to the next field */
            while (pos < flen && !using_is_num_start(fmt + pos) && !using_is_str_start(fmt + pos)) {
                if (fmt[pos] == '_' && pos + 1 < flen) { EMIT(fmt[pos + 1]); pos += 2; }
                else EMIT(fmt[pos++]);
            }
            break;
        }
    }
    #undef EMIT
    out[o] = '\0';
    display_print(out);
    if (!trailing) display_newline();
    return p;
}

/* ================================================================
 * Graphics backend  two compile paths:
 *
 *   USE_SDL_WINDOW   call basic_gfx.h functions directly (SDL pixel buffer)
 *   (default)        emit OSC 666 escape sequences to stdout (Felix terminal)
 * ================================================================ */

/* Current screen state  updated by SCREEN, read by graphics cmds */
int g_screen_mode   = 0;
int g_screen_width  = 640;
int g_screen_height = 350;
int g_back_color    = 0;   /* palette index used by CLS */

#ifdef USE_SDL_WINDOW
/*  SDL direct path  */
#include "basic_gfx.h"
#include "basic_gfx_sdl.h"

/* No-op shims so the rest of the file compiles unchanged */
static void felix_send(char *)  {}
static void felix_sendf(char *, ...) {}
static void felix_draw(char *)  {}
static void felix_drawf(char *, ...) {}

#else
/*  OSC 666 escape-code path (original Felix terminal protocol)  */
#if defined(_MSC_VER) && !defined(__MINGW32__)
#include "msvc_posix_compat.h"  // unistd.h doesn't exist under MSVC; write/STDOUT_FILENO
#else
#include <unistd.h>
#endif

static void felix_send(char *cmd) {
    write(STDOUT_FILENO, "\033]666;", 6);
    write(STDOUT_FILENO, cmd, strlen(cmd));
    write(STDOUT_FILENO, "\033\\", 2);
}

static void felix_sendf(char *fmt, ...) {
    char buf[DEFAULT_BUFFER];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    felix_send(buf);
}

/* Draw commands are wrapped in "batch;" so the terminal flushes them
 * together on the next frame. */
static void felix_draw(char *cmd) {
    write(STDOUT_FILENO, "\033]666;batch;", 12);
    write(STDOUT_FILENO, cmd, strlen(cmd));
    write(STDOUT_FILENO, "\033\\", 2);
}

static void felix_drawf(char *fmt, ...) {
    char buf[DEFAULT_BUFFER];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    felix_draw(buf);
}
#endif /* USE_SDL_WINDOW */
 
static void ega_to_rgb(int ega6, int *r, int *g, int *b) {
    /* QB SCREEN 9 PALETTE color-number to RGB.
     * Colors 0-15: standard CGA 16-color table (hardcoded, includes brown/gray).
     * Colors 16-63: EGA extended palette.
     *   Primary bits  (2,1,0) contribute 0xAA (2/3 intensity) to R,G,B.
     *   Secondary bits (5,4,3) contribute 0x55 (1/3 intensity) to R,G,B.
     * Formula: r_bits = (bit2<<1)|bit5, then r = r_bits*85.
     * Gives EGA 46 -> #FFAA55 (orange) as expected for gorilla.bas. */
    static const int cga16[16][3] = {
        {0,0,0},{0,0,0xAA},{0,0xAA,0},{0,0xAA,0xAA},
        {0xAA,0,0},{0xAA,0,0xAA},{0xAA,0x55,0},{0xAA,0xAA,0xAA},
        {0x55,0x55,0x55},{0x55,0x55,0xFF},{0x55,0xFF,0x55},{0x55,0xFF,0xFF},
        {0xFF,0x55,0x55},{0xFF,0x55,0xFF},{0xFF,0xFF,0x55},{0xFF,0xFF,0xFF}
    };
    if (ega6 >= 0 && ega6 < 16) {
        *r = cga16[ega6][0]; *g = cga16[ega6][1]; *b = cga16[ega6][2];
        return;
    }
    /* Extended colors 16-63: primary bits at 2,1,0 (high); secondary at 5,4,3 (low) */
    int rb = (((ega6>>2)&1)<<1) | ((ega6>>5)&1);
    int gb = (((ega6>>1)&1)<<1) | ((ega6>>4)&1);
    int bb = (((ega6>>0)&1)<<1) | ((ega6>>3)&1);
    *r = rb * 85; *g = gb * 85; *b = bb * 85;
}

/* Screen mode  (width, height) */
static void screen_dims(int mode, int *w, int *h) {
    static const struct { int m, w, h; } modes[] = {
        {1,320,200},{2,640,200},{3,720,348},{4,640,400},{5,160,100},
        {6,160,200},{7,320,200},{8,640,200},{9,640,350},{10,640,350},
        {11,640,480},{12,640,480},{13,320,200},{14,320,200},{15,640,200},
        {16,640,480},{17,640,480},{18,640,480},{19,640,480},{20,512,480},
        {21,640,400},{22,640,480},{23,800,600},{24,160,200},{25,320,200},
        {26,640,200},{27,640,200},{28,720,350},{0,0,0}
    };
    for (int i = 0; modes[i].m || modes[i].w; i++) {
        if (modes[i].m == mode) { *w = modes[i].w; *h = modes[i].h; return; }
    }
    *w = 640; *h = 350;
}

/* Sprite ID registry: maps array variable pointer  sprite ID */
#define MAX_SPRITES 64
static struct { Var *var; int id; } g_sprites[MAX_SPRITES];
static int g_nsprites = 0;
static int g_next_sprite_id = 1;

void sprites_reset(void) {
    g_nsprites = 0;
    g_next_sprite_id = 1;
#ifdef USE_SDL_WINDOW
    gfx_sprites_clear();
#endif
}

/* A variable going away (a procedure's, when it returns): its slot may be
 * reused, so it no longer names its sprite. */
void sprite_forget(Var *var) {
    for (int i = 0; i < g_nsprites; i++)
        if (g_sprites[i].var == var) {
            g_sprites[i] = g_sprites[--g_nsprites];
            return;
        }
}

static int sprite_id_for(Var *var) {
    for (int i = 0; i < g_nsprites; i++)
        if (g_sprites[i].var == var) return g_sprites[i].id;
    if (g_nsprites >= MAX_SPRITES) return -1;
    int id = g_next_sprite_id++;
    g_sprites[g_nsprites].var = var;
    g_sprites[g_nsprites].id  = id;
    g_nsprites++;
    return id;
}

/* Parse  (x, y)  or  x, y  returning pointer past closing paren (if any) */
static char *parse_xy(char *p, double *x, double *y) {
    int paren = (*p == '(');
    if (paren) p = sk(p + 1);
    mpf_t mx, my; mpf_init2(mx, g_prec); mpf_init2(my, g_prec);
    p = sk(eval_expr(p, mx)); *x = mpf_get_d(mx); mpf_clear(mx);
    if (*p == ',') p = sk(p + 1);
    p = sk(eval_expr(p, my)); *y = mpf_get_d(my); mpf_clear(my);
    if (paren && *p == ')') p = sk(p + 1);
    return p;
}

/* No-op stubs */
static int cmd_rem(Interp *ip, char *args)    { (void)ip;(void)args; return 0; }
static int cmd_defseg(Interp *ip, char *args) { (void)ip;(void)args; return 0; }
static int cmd_defdbl(Interp *ip, char *args) { (void)ip;(void)args; return 0; }
static int cmd_key(Interp *ip, char *args)    { (void)ip;(void)args; return 0; }
/* ----------------------------------------------------------------
 * cmd_run  RUN [linenum | "filename"]
 *
 *   RUN              reset vars/stack/data, run from the first line
 *   RUN 500          reset, run from line 500
 *   RUN "game.bas"   load file, reset, run from start
 *                    (identical to CHAIN "game.bas")
 * ---------------------------------------------------------------- */
static int cmd_run(Interp *ip, char *args) {
    char *p = sk(args);

    if (*p == '"' || *p == '\'') {
        /* RUN "filename"  load file then run it */
        char name[DEFAULT_BUFFER];
        char q = *p++;
        int ni = 0;
        while (*p && *p != q && ni < (int)sizeof(name) - 1)
            name[ni++] = *p++;
        name[ni] = '\0';
        if (!*name) { display_print("RUN: missing filename\n"); return 0; }
        load_program(name);
        prescan_data();
        g_nvar     = 0;
        scope_program_start();
        g_ctrl_top = 0;
        g_data_pos = 0;
        ip->pc     = 0;
        return 1;
    }

    /* Reset interpreter state (variables, stack, data pointer) */
    g_nvar     = 0;
    scope_program_start();
    g_ctrl_top = 0;
    g_data_pos = 0;

    if (*p && (isdigit((unsigned char)*p) || *p == '-')) {
        /* RUN linenum */
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        int linenum = (int)mpf_get_si(n);
        mpf_clear(n);
        int idx = find_line_idx(linenum);
        if (idx < 0) {
            basic_stderr("RUN: line %d not found\n", linenum);
            ip->running = 0;
            return 1;
        }
        ip->pc = idx;
    } else {
        ip->pc = 0;
    }
    return 1;   /* tell the run loop we set ip->pc (jumped) */
}

static int cmd_chain(Interp *ip, char *args) { return cmd_run(ip, args); }
static int cmd_fullscreen(Interp *ip, char *args) {
    (void)ip;
#ifdef USE_SDL_WINDOW
    char *p = sk(args);
    if (*p == '\0' || *p == ':') {
        // No argument  always enter fullscreen
        gfx_sdl_set_fullscreen(true);
    } else {
        // Argument: 0 = windowed, non-zero = fullscreen
        mpf_t v; mpf_init2(v, g_prec);
        eval_expr(p, v);
        bool fs = (mpf_get_d(v) != 0.0);
        mpf_clear(v);
        gfx_sdl_set_fullscreen(fs);
    }
#else
    (void)args;
#endif
    return 0;
}

/* DELAY t# / SLEEP t#  pause for t# seconds (QB64 built-ins) */
static void basic_wait(double secs);
static int cmd_delay(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!*p || *p == ':') return 0;
    mpf_t n; mpf_init2(n, g_prec);
    eval_expr(p, n);
    double secs = mpf_get_d(n);
    mpf_clear(n);
    if (secs > 0) basic_wait(secs);
    return 0;
}

/* ================================================================
 * SCREEN mode
 * ================================================================ */


static int cmd_screen(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!*p || *p == ':') return 0;

    /* Parse up to 4 args: mode [, colorswitch [, apage [, vpage]]] */
    auto next_arg = [&](long def) -> long {
        if (*p == ',') {
            p = sk(p + 1);
            if (*p == ',' || *p == ':' || *p == '\00') return def;
            mpf_t n; mpf_init2(n, g_prec);
            p = sk(eval_expr(p, n));
            long v = mpf_get_si(n); mpf_clear(n);
            return v;
        }
        return def;
    };

    mpf_t n; mpf_init2(n, g_prec);
    p = sk(eval_expr(p, n));
    long encoded = mpf_get_si(n);
    mpf_clear(n);

    long colorswitch = next_arg(0);
    long apage       = next_arg(-1);   /* -1 = unchanged */
    long vpage       = next_arg(-1);   /* -1 = unchanged */

    if (encoded < 0) {
        /* Truecolor sentinel from _NEWIMAGE(w,h,32): encoded = -(w*100000+h) */
        long val = -encoded;
        int tw = (int)(val / 100000L);
        int th = (int)(val % 100000L);
        g_screen_mode = -1;
        g_screen_width  = tw;
        g_screen_height = th;
#ifdef USE_SDL_WINDOW
        gfx_screen_tc(tw, th);
#else
        felix_sendf("screen;12");
#endif
    } else {
        int mode = (int)encoded;
        g_screen_mode = mode;
        screen_dims(mode, &g_screen_width, &g_screen_height);
#ifdef USE_SDL_WINDOW
        ::gfx_screen_ex(mode, (int)colorswitch, (int)apage, (int)vpage);
        g_gfx_x = 0.0; g_gfx_y = 0.0;  /* reset Current Graphics Position */
#else
        if (mode == 0) felix_draw("cls;0");
        else           felix_sendf("screen;%d", mode);
#endif
    }
    return 0;
}
/* WINDOW (x1, y1)-(x2, y2) — QB64 logical coordinate system (stub) */
static int cmd_window(Interp *ip, char *args) {
    (void)ip; (void)args;
    /* Just consume the arguments and do nothing */
    return 0;
}

static int cmd_qdisplay(Interp *ip, char *args) {
    (void)ip; (void)args;
#ifdef USE_SDL_WINDOW
    ::gfx_sdl_render();
#endif
    return 0;
}

static int cmd_qtitle(Interp *ip, char *args) {
    (void)ip; (void)args;
    /* Just skip the string - don't evaluate it */
    return 0;
}

/* _LIMIT fps — QB64 frame rate limiter */
static int cmd_qlimit(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t fps_val; mpf_init2(fps_val, g_prec);
    p = sk(eval_expr(p, fps_val));
    double fps = mpf_get_d(fps_val);
    mpf_clear(fps_val);
    
    if (fps > 0) {
        /* Sleep to maintain target frame rate */
        static uint32_t last_time = 0;
        uint32_t now = SDL_GetTicks();
        if (last_time > 0) {
            uint32_t frame_time = (uint32_t)(1000.0 / fps);
            uint32_t elapsed = now - last_time;
            if (elapsed < frame_time) basic_wait((frame_time - elapsed) / 1000.0);
        }
        last_time = SDL_GetTicks();
    }
    return 0;
}

static int cmd_beep(Interp *ip, char *args) {
    (void)ip; (void)args;
    sound_beep();
    return 0;
}

/* SOUND freq, duration   freq in Hz, duration in 18.2-tick clock units */
static int cmd_sound(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t freq, dur; mpf_init2(freq, g_prec); mpf_init2(dur, g_prec);
    p = sk(eval_expr(p, freq));
    if (*p == ',') p = sk(p + 1);
    eval_expr(p, dur);
    sound_tone(mpf_get_d(freq), mpf_get_d(dur));
    mpf_clears(freq, dur, NULL);
    return 0;
}

/* PLAY "mml-string"  GW-BASIC Music Macro Language */
static int cmd_play(Interp *ip, char *args) {
    (void)ip;
    char mml[1024];
    eval_str_expr(sk(args), mml, sizeof mml);
    sound_play(mml);
    return 0;
}

/* ================================================================
 * Interpreter control
 * ================================================================ */
static int cmd_stop(Interp *ip, char *args) {
    (void)args;
    g_cont_pc = ip->pc + 1;
    ip->running = 0;
    display_print("\nBreak\n");
    return 1;
}

static int cmd_cont(Interp *ip, char *args) {
    (void)args;
    if (g_cont_pc < 0) { display_print("Can't continue\n"); return 0; }
    ip->pc = g_cont_pc;
    g_cont_pc = -1;
    return 1;
}

static int cmd_end(Interp *ip, char *args) {
    (void)args; ip->running = 0;
    for (int i = 1; i <= MAX_FILE_HANDLES; i++)
        if (g_files[i].fp) { fclose(g_files[i].fp); g_files[i].fp = NULL; g_files[i].mode = 0; }
    return 1;
}

/* ================================================================
 * Utility commands
 * ================================================================ */
static int cmd_randomize(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p && *p != ':') {
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        srand((unsigned)mpf_get_si(n));
        mpf_clear(n);
    } else {
        srand((unsigned)time(NULL));
    }
    return 0;
}

static int cmd_swap(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    char name1[MAX_VARNAME], name2[MAX_VARNAME];
    p = sk(read_varname(p, name1));
    if (*p == ',') p = sk(p + 1);
    read_varname(p, name2);
    Var *a = var_get(name1), *b = var_get(name2);
    if (var_is_str_name(name1)) {
        char *tmp = a->str; a->str = b->str; b->str = tmp;
    } else {
        mpf_t tmp; mpf_init2(tmp, g_prec);
        mpf_set(tmp, a->num); mpf_set(a->num, b->num); mpf_set(b->num, tmp);
        mpf_clear(tmp);
    }
    return 0;
}

static int cmd_erase(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(p, name));
        Var *v = var_find(name);
        if (v) {
            var_free_arrays(v);
            v->kind = var_is_str_name(name) ? VAR_STR : VAR_NUM;
            v->ndim = 0;
        }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

static int cmd_option(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (kw_match(p, "_EXPLICIT")) {
        /* QB64 option for strict variable declaration — just ignore it */
        return 0;
    }
    if (kw_match(p, "BASE")) {
        p = sk(p + 4);
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        int base = (int)mpf_get_si(n); mpf_clear(n);
        if (base == 0 || base == 1) g_option_base = base;
        else basic_stderr("OPTION BASE must be 0 or 1\n");
    }
    return 0;
}

/* ================================================================
 * Display commands
 * ================================================================ */
static int cmd_system(Interp *ip, char *args) {
    (void)args;
    ip->running = 0;
    /* longjmp out if available (WOPR context), otherwise just stop */
#ifdef INLINEBASIC
    extern void basic_exit_(void);
    basic_exit_();
#endif
    return 1;
}

/* Show what's been drawn and keep the window alive while a program waits
 * (DELAY, SLEEP, _LIMIT), so animation paced by them is seen frame by
 * frame. Ctrl+Break ends the wait. */
#ifdef USE_SDL_WINDOW
static Uint32 s_last_wait_ms;      /* when the program last paused */
#endif

/* True while the program paces itself with pauses: then the screen is shown
 * at each pause, when a frame is complete, and not also at odd moments in
 * between (half-drawn, say between erasing a sprite and drawing it again). */
int basic_paced(void) {
#ifdef USE_SDL_WINDOW
    return s_last_wait_ms && SDL_GetTicks() - s_last_wait_ms < 250;
#else
    return 0;
#endif
}

static void basic_wait(double secs) {
#ifdef USE_SDL_WINDOW
    ::gfx_sdl_pump();
    ::gfx_sdl_render();
    Uint32 end = SDL_GetTicks() + (Uint32)(secs * 1000.0 + 0.5);
    while (!g_break) {
        Sint32 left = (Sint32)(end - SDL_GetTicks());
        if (left <= 0) break;
        SDL_Delay(left > 10 ? 10 : (Uint32)left);
        ::gfx_sdl_pump();
    }
    s_last_wait_ms = SDL_GetTicks();
    if (!s_last_wait_ms) s_last_wait_ms = 1;
#elif defined(_WIN32)
    Sleep((DWORD)(secs * 1000));
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

/* Called between statements run outside the main loop (FUNCTION bodies):
 * present the screen every 16 ms, as the main loop does. */
void basic_frame_tick(void) {
#ifdef USE_SDL_WINDOW
    static Uint32 last = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last >= 16) {
        ::gfx_sdl_pump();
        if (!basic_paced()) ::gfx_sdl_render();
        last = now;
    }
#endif
}

static int cmd_sleep(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double secs = 1.0;
    if (*p && *p != ':') {
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        secs = mpf_get_d(n);
        mpf_clear(n);
    }
    if (secs > 0) basic_wait(secs);
    return 0;
}

static int cmd_kill(Interp *ip, char *args) {
    (void)ip;
    char filename[DEFAULT_BUFFER] = "";
    char *p = sk(args);
    if (*p == '"') p++;
    int i = 0;
    while (*p && *p != '"' && i < (int)sizeof(filename) - 1) filename[i++] = *p++;
    filename[i] = '\0';
    if (filename[0]) remove(filename);
    return 0;
}

static int g_view_top = 1, g_view_bottom = 25;   /* the text viewport (VIEW PRINT) */
static int cmd_cls(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    int arg = -1;  /* -1 = no argument */
#ifndef USE_SDL_WINDOW
    printf("\033[2J\033[H");
    fflush(stdout);
#endif
    if (*p && *p != ':') {
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        arg = (int)mpf_get_si(n);
        mpf_clear(n);
    }
        /* QB CLS semantics:
         *   CLS 1 = clear graphics viewport only
         *   CLS 2 = clear text viewport only (no-op in pixel-buffer mode)
         *   CLS (no arg) = clear both
         * The arg is NOT a color index. Always clear to background (index 0). */
        if (arg == 2) {
            /* the text viewport (VIEW PRINT), with the graphics under it */
#ifdef USE_SDL_WINDOW
            gfx_cls_text(g_view_top, g_view_bottom);
#endif
            return 0;
        }
        /* arg==1 or no arg: clear graphics to background color */
        int c = 0;  /* always clear pixel buffer to color index 0 (background) */
        (void)arg;
#ifdef USE_SDL_WINDOW
        gfx_cls(c);
        /* display_cls() intentionally NOT called here  gfx_cls() already
         * clears both the pixel buffer and the text grid, and display_cls()
         * would overwrite with s_cur_bg (color 0), losing the CLS argument. */
#else
        if (c == 0) felix_draw("cls;0");
        else        felix_drawf("cls;%d", c);
        display_cls();
#endif
    return 0;
}

static int cmd_width(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    int cols = display_get_width();
    if (*p != ',') {
        mpf_t n; mpf_init2(n, g_prec);
        p = sk(eval_expr(p, n));
        cols = (int)mpf_get_si(n);
        mpf_clear(n);
    }
    /* optional second argument: rows */
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t r; mpf_init2(r, g_prec);
        eval_expr(p, r);
        display_text_rows((int)mpf_get_si(r));
        mpf_clear(r);
    }
    display_width(cols);
    return 0;
}

static int cmd_color(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    /* COLOR fg / COLOR , bg: what's left out stays as it was */
    static int cur_fg = 7, cur_bg = 0;
    mpf_t fg, bg; mpf_init2(fg, g_prec); mpf_init2(bg, g_prec);
    mpf_set_si(fg, cur_fg); mpf_set_si(bg, cur_bg);
    if (!*p) { mpf_clears(fg, bg, NULL); return 0; } /* bare COLOR  no-op */
    if (*p != ',') p = eval_expr(p, fg);
    p = sk(p);
    if (*p == ',') { p = sk(p + 1); if (*p) p = eval_expr(p, bg); }
    cur_fg = (int)mpf_get_si(fg); cur_bg = (int)mpf_get_si(bg);
    display_color(cur_fg, cur_bg);
    mpf_clears(fg, bg, NULL);
    return 0;
}

static int cmd_locate(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t row, col, cur;
    mpf_init2(row, g_prec); mpf_init2(col, g_prec); mpf_init2(cur, g_prec);
    mpf_set_ui(row, 0); mpf_set_ui(col, 0); mpf_set_ui(cur, 1);

    int has_cur = 0;
    if (*p && *p != ',') p = sk(eval_expr(p, row));
    if (*p == ',') p = sk(p + 1); else goto locate_done;
    if (*p && *p != ',') p = sk(eval_expr(p, col));
    if (*p == ',') p = sk(p + 1); else goto locate_done;
    if (*p && *p != ',') { p = sk(eval_expr(p, cur)); has_cur = 1; }

    locate_done:
    if (mpf_get_si(row) > 0 || mpf_get_si(col) > 0)
        display_locate((int)mpf_get_si(row), (int)mpf_get_si(col));
    if (has_cur) display_cursor((int)mpf_get_si(cur));
    mpf_clears(row, col, cur, NULL);
    return 0;
}

/* ================================================================
 * DO ... LOOP [WHILE|UNTIL cond]
 * WHILE cond ... WEND
 *
 * Both use the control stack with a special frame tag.
 * The loop start pc is stored; on LOOP/WEND we re-evaluate and jump back.
 * ================================================================ */

/* Forward declarations for commands used before their definition */
static int cmd_dim(Interp *ip, char *args);
static int cmd_return(Interp *ip, char *args);
static void byref_return(int fi);
typedef struct { char base[MAX_VARNAME], field[MAX_VARNAME]; int nidx, i, j; } FieldRef;
static char *parse_field_varname(char *p, char *out, FieldRef *fr);
static int eval_one_cmp(char **pp);

/* Evaluate a full boolean expression: one or more comparisons joined by AND/OR */
/* Parse one AND-chain: cmp (AND cmp)*
 * AND binds tighter than OR, so this is called per-term by eval_bool_expr. */
static int eval_and_chain(char **pp) {
    int result = eval_one_cmp(pp);
    char *p = sk(*pp);
    while (kw_match(p, "AND")) {
        p = sk(p + 3);
        int next = eval_one_cmp(&p);
        result = result && next;
        p = sk(p);
    }
    *pp = p;
    return result;
}

/* Evaluate a full boolean expression: AND-chains joined by OR.
 * Precedence: AND > OR  (matches QBasic / standard BASIC behaviour) */
static int eval_bool_expr(char **pp) {
    int result = eval_and_chain(pp);
    char *p = sk(*pp);
    while (kw_match(p, "OR")) {
        p = sk(p + 2);
        int next = eval_and_chain(&p);
        result = result || next;
        p = sk(p);
    }
    *pp = p;
    return result;
}

/* DO [WHILE|UNTIL cond]  push a loop frame pointing at the DO statement */
static int cmd_do(Interp *ip, char *args) {
    /* When LOOP sends us back here, the frame already exists  don't push again.
     * But we must still re-check any DO WHILE / DO UNTIL condition. */
    if (g_ctrl_top > 0 &&
        strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x02" "DO") == 0 &&
        g_ctrl[g_ctrl_top - 1].line_idx == ip->pc) {
        char *p = sk(args);
        if (kw_match(p, "WHILE") || kw_match(p, "UNTIL")) {
            int is_until = kw_match(p, "UNTIL");
            p = sk(p + 5);
            int cond = eval_bool_expr(&p);
            if (is_until) cond = !cond;
            if (!cond) {
                /* skip to after matching LOOP */
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    char *t = sk(g_lines[pc].text);
                    if (kw_match(t, "DO")) depth++;
                    else if (kw_match(t, "LOOP")) depth--;
                    pc++;
                }
                mpf_clear(g_ctrl[g_ctrl_top - 1].limit);
                mpf_clear(g_ctrl[g_ctrl_top - 1].step);
                g_ctrl_top--;
                ip->pc = pc; return 1;
            }
        }
        return 0;
    }

    /* First entry: optionally check DO WHILE/UNTIL before pushing */
    char *p = sk(args);
    if (kw_match(p, "WHILE") || kw_match(p, "UNTIL")) {
        int is_until = kw_match(p, "UNTIL");
        p = sk(p + 5);
        int cond = eval_bool_expr(&p);
        if (is_until) cond = !cond;
        if (!cond) {
            int depth = 1, pc = ip->pc + 1;
            while (pc < g_nlines && depth > 0) {
                char *t = sk(g_lines[pc].text);
                if (kw_match(t, "DO")) depth++;
                else if (kw_match(t, "LOOP")) depth--;
                pc++;
            }
            ip->pc = pc; return 1;
        }
    }
    if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stacktrace("Stack overflow"); return -1; }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strcpy(f->varname, "\x02" "DO");
    f->line_idx = ip->pc;
    mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
    mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
    return 0;
}

/* LOOP [WHILE|UNTIL cond] */
static int cmd_loop(Interp *ip, char *args) {
    /* find the matching DO frame */
    int fi = g_ctrl_top - 1;
    while (fi >= 0 && strcmp(g_ctrl[fi].varname, "\x02" "DO") != 0) fi--;
    if (fi < 0) { basic_stderr("LOOP without DO\n"); return -1; }

    char *p = sk(args);
    int keep_looping = 1;

    if (kw_match(p, "WHILE")) {
        p = sk(p + 5);
        keep_looping = eval_bool_expr(&p);
    } else if (kw_match(p, "UNTIL")) {
        p = sk(p + 5);
        int cond = eval_bool_expr(&p);
        keep_looping = !cond;
    }

    if (keep_looping) {
#ifdef USE_SDL_WINDOW
        /* Cap DO loops to 60 fps so animation is always visible regardless
         * of how fast or slow the loop body runs.  The limit field of the
         * DO frame is unused; we repurpose it as a millisecond timestamp. */
        {
            Uint32 now = SDL_GetTicks();
            Uint32 last = (Uint32)mpf_get_ui(g_ctrl[fi].limit);
            if (last == 0 || now - last >= 16) {
                mpf_set_ui(g_ctrl[fi].limit, (unsigned long)now);
                ::gfx_sdl_pump();
                if (!basic_paced()) ::gfx_sdl_render();
            }
        }
#endif
        ip->pc = g_ctrl[fi].line_idx;
        return 1;
    } else {
        mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
        g_ctrl_top = fi;
        return 0;
    }
}

/* WHILE cond */
static int cmd_while(Interp *ip, char *args) {
    char *p = sk(args);
    int cond = eval_one_cmp(&p);
    p = sk(p);
    while (kw_match(p,"AND") || kw_match(p,"OR")) {
        int is_and = kw_match(p,"AND");
        p = sk(p + (is_and ? 3 : 2));
        int next = eval_one_cmp(&p);
        cond = is_and ? (cond && next) : (cond || next);
        p = sk(p);
    }

    if (cond) {
        /* Only push a new frame if we're not already tracking this WHILE */
        int already = (g_ctrl_top > 0 &&
                       strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x03" "WHILE") == 0 &&
                       g_ctrl[g_ctrl_top - 1].line_idx == ip->pc);
        if (!already) {
            if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stacktrace("Stack overflow"); return -1; }
            CtrlFrame *f = &g_ctrl[g_ctrl_top++];
            strcpy(f->varname, "\x03" "WHILE");
            f->line_idx = ip->pc;
            mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
            mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
        }
        return 0;
    } else {
        /* Condition false: pop frame if present, skip to WEND */
        if (g_ctrl_top > 0 &&
            strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x03" "WHILE") == 0 &&
            g_ctrl[g_ctrl_top - 1].line_idx == ip->pc) {
            mpf_clear(g_ctrl[g_ctrl_top - 1].limit);
            mpf_clear(g_ctrl[g_ctrl_top - 1].step);
            g_ctrl_top--;
        }
        int depth = 1;
        int pc = ip->pc + 1;
        while (pc < g_nlines && depth > 0) {
            char *t = sk(g_lines[pc].text);
            if (kw_match(t, "WHILE")) depth++;
            else if (kw_match(t, "WEND")) depth--;
            if (depth > 0) pc++;
        }
        ip->pc = pc + 1;
        return 1;
    }
}

/* WEND  jump back to the matching WHILE; WHILE will pop if condition fails */
static int cmd_wend(Interp *ip, char *args) {
    (void)args;
    int fi = g_ctrl_top - 1;
    while (fi >= 0 && strcmp(g_ctrl[fi].varname, "\x03" "WHILE") != 0) fi--;
    if (fi < 0) { basic_stderr("WEND without WHILE\n"); return -1; }
    ip->pc = g_ctrl[fi].line_idx;   /* jump to WHILE line  it re-evaluates & pops if done */
    return 1;
}

/* ================================================================
 * SELECT CASE expr
 *   CASE val [, val ...]
 *   CASE IS op val
 *   CASE val TO val
 *   CASE ELSE
 * END SELECT
 * ================================================================ */

/* Forward-scan to find the next CASE or END SELECT at the same depth */
static int find_next_case(int start_pc) {
    int depth = 0;
    for (int pc = start_pc; pc < g_nlines; pc++) {
        char *t = sk(g_lines[pc].text);
        if (kw_match(t, "SELECT")) depth++;
        else if (kw_match(t, "END") && kw_match(sk(t + 3), "SELECT")) {
            if (depth == 0) return pc;
            depth--;
        } else if (depth == 0 && kw_match(t, "CASE")) return pc;
    }
    return g_nlines;
}

static int cmd_select(Interp *ip, char *args) {
    char *p = sk(args);
    /* skip optional CASE keyword after SELECT */
    if (kw_match(p, "CASE")) p = sk(p + 4);

    /* Evaluate the selector  could be string or numeric */
    char sel_s[1024] = ""; double sel_n = 0; int sel_is_str = 0;
    if (is_str_token(p)) {
        eval_str_expr(p, sel_s, sizeof sel_s);
        sel_is_str = 1;
    } else {
        mpf_t v; mpf_init2(v, g_prec);
        eval_expr(p, v);
        sel_n = mpf_get_d(v);
        mpf_clear(v);
    }

    /* Scan forward through CASE clauses */
    int pc = ip->pc + 1;
    while (pc < g_nlines) {
        char *t = sk(g_lines[pc].text);

        if (kw_match(t, "END") && kw_match(sk(t + 3), "SELECT")) {
            ip->pc = pc + 1; return 1;    /* no match  skip to END SELECT */
        }

        if (!kw_match(t, "CASE")) { pc++; continue; }

        t = sk(t + 4);   /* past CASE */

        /* CASE ELSE always matches */
        if (kw_match(t, "ELSE")) { ip->pc = pc + 1; return 1; }

        /* Try each comma-separated value list */
        int matched = 0;
        while (*t && !matched) {
            t = sk(t);
            if (sel_is_str) {
                char cval[1024];
                t = eval_str_expr(t, cval, sizeof cval);
                t = sk(t);
                if (kw_match(t, "TO")) {
                    t = sk(t + 2);
                    char cval2[1024];
                    t = eval_str_expr(t, cval2, sizeof cval2);
                    matched = (strcmp(sel_s, cval) >= 0 && strcmp(sel_s, cval2) <= 0);
                } else {
                    matched = (strcmp(sel_s, cval) == 0);
                }
            } else if (kw_match(t, "IS")) {
                /* CASE IS op val */
                t = sk(t + 2);
                char op[3] = {t[0], t[0] ? t[1] : '\0', '\0'}; int ol = 2;
                if (!strcmp(op,"<>")||!strcmp(op,"><")||!strcmp(op,"<=")||
                    !strcmp(op,"=<")||!strcmp(op,">=")||!strcmp(op,"=>")) ;
                else { op[1] = '\0'; ol = 1; }
                t = sk(t + ol);
                mpf_t cv; mpf_init2(cv, g_prec);
                t = eval_expr(t, cv);
                double cv_d = mpf_get_d(cv); mpf_clear(cv);
                if      (!strcmp(op,"<>")||!strcmp(op,"><")) matched=(sel_n!=cv_d);
                else if (!strcmp(op,"<=")||!strcmp(op,"=<")) matched=(sel_n<=cv_d);
                else if (!strcmp(op,">=")||!strcmp(op,"=>")) matched=(sel_n>=cv_d);
                else if (op[0]=='<') matched=(sel_n<cv_d);
                else if (op[0]=='>') matched=(sel_n>cv_d);
                else                 matched=(sel_n==cv_d);
            } else {
                mpf_t cv; mpf_init2(cv, g_prec);
                t = eval_expr(t, cv);
                double lo = mpf_get_d(cv); mpf_clear(cv);
                t = sk(t);
                if (kw_match(t, "TO")) {
                    t = sk(t + 2);
                    mpf_t cv2; mpf_init2(cv2, g_prec);
                    t = eval_expr(t, cv2);
                    double hi = mpf_get_d(cv2); mpf_clear(cv2);
                    matched = (sel_n >= lo && sel_n <= hi);
                } else {
                    matched = (sel_n == lo);
                }
            }
            t = sk(t);
            if (*t == ',') t = sk(t + 1); else break;
        }

        if (matched) { ip->pc = pc + 1; return 1; }
        pc = find_next_case(pc + 1);
    }
    ip->pc = pc + 1; return 1;
}

static int cmd_case(Interp *ip, char *args) {
    (void)args;
    /* Reached a CASE line during normal execution  a prior CASE body just
     * finished.  Jump forward to END SELECT at the same nesting depth. */
    int depth = 0, pc = ip->pc + 1;
    while (pc < g_nlines) {
        char *t = sk(g_lines[pc].text);
        if (kw_match(t, "SELECT")) depth++;
        else if (kw_match(t, "END") && kw_match(sk(t + 3), "SELECT")) {
            if (depth == 0) { ip->pc = pc + 1; return 1; }
            depth--;
        }
        pc++;
    }
    ip->pc = g_nlines;
    return 1;
}

static int cmd_end_select(Interp *ip, char *args) {
    (void)ip; (void)args; return 0;
}

/* ================================================================
 * EXIT SUB / EXIT FUNCTION / EXIT FOR / EXIT DO
 * ================================================================ */
static int cmd_exit(Interp *ip, char *args) {
    char *p = sk(args);
    if (kw_match(p, "FOR")) {
        /* pop to the innermost FOR frame */
        for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
            if (g_ctrl[fi].varname[0] != '\x01' &&
                g_ctrl[fi].varname[0] != '\x02' &&
                g_ctrl[fi].varname[0] != '\x03') {
                /* scan forward to NEXT */
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    char *t = sk(g_lines[pc].text);
                    if (kw_match(t, "FOR")) depth++;
                    else if (kw_match(t, "NEXT")) depth--;
                    pc++;
                }
                mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
                g_ctrl_top = fi;
                ip->pc = pc; return 1;
            }
        }
    } else if (kw_match(p, "DO")) {
        /* pop to innermost DO frame, scan forward to LOOP */
        for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
            if (strcmp(g_ctrl[fi].varname, "\x02" "DO") == 0) {
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    char *t = sk(g_lines[pc].text);
                    if (kw_match(t, "DO")) depth++;
                    else if (kw_match(t, "LOOP")) depth--;
                    pc++;
                }
                mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
                g_ctrl_top = fi;
                ip->pc = pc; return 1;
            }
        }
    } else if (kw_match(p, "SUB") || kw_match(p, "FUNCTION")) {
        /* unwind to the nearest GOSUB return frame */
        for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
            if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0) {
                byref_return(fi);
                ip->pc = g_ctrl[fi].line_idx;
                mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
                g_ctrl_top = fi;
                return 1;
            }
        }
        ip->running = 0; return 1;
    }
    return 0;
}

/* ================================================================
 * REDIM  same as DIM for our purposes (we don't track initialisation)
 * ================================================================ */
static int cmd_redim(Interp *ip, char *args) {
    return cmd_dim(ip, args);
}

/* ================================================================
 * ON ERROR GOTO / RESUME  stub implementations
 * We don't support full error trapping, but we need these to not crash.
 * ON ERROR GOTO 0 disables any pending handler (no-op for us).
 * RESUME NEXT advances past the erroring line (no-op in stub).
 * ================================================================ */
static int cmd_on_error(Interp *ip, char *args) {
    char *p = sk(args);
    if (kw_match(p, "GOTO")) {
        p = sk(p + 4);
        if (*p == '0' && !isalnum((unsigned char)p[1])) {
            /* ON ERROR GOTO 0  disable handler */
            g_error_handler[0] = '\0';
            return 0;
        }
        /* Register handler label/line  do NOT jump now */
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < MAX_VARNAME - 1)
            g_error_handler[i++] = *p++;
        g_error_handler[i] = '\0';
        return 0;
    }
    return 0;
}

static int cmd_resume(Interp *ip, char *args) {
    char *p = sk(args);
    if (kw_match(p, "NEXT")) {
        if (g_error_resume_pc >= 0) {
            ip->pc = g_error_resume_pc + 1;
            g_error_resume_pc = -1;
            return 1;
        }
        return 0;
    }
    /* RESUME [line]  retry the line that caused the error, or jump to line */
    p = sk(p);
    if (isdigit((unsigned char)*p) || isalpha((unsigned char)*p)) {
        /* RESUME line_number  jump there */
        char target[MAX_VARNAME]; int i = 0;
        while ((*p && !isspace((unsigned char)*p)) && i < MAX_VARNAME-1)
            target[i++] = *p++;
        target[i] = '\0';
        g_error_resume_pc = -1;
        return cmd_goto(ip, target);
    }
    if (g_error_resume_pc >= 0) {
        ip->pc = g_error_resume_pc;
        g_error_resume_pc = -1;
        return 1;
    }
    return 0;
}

/* ================================================================
 * WRITE  like PRINT but comma-separated with strings quoted
 * ================================================================ */
static int cmd_write(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    /* WRITE #n  file output */
    if (*p == '#') {
        p = sk(p + 1);
        mpf_t n; mpf_init2(n, g_prec);
        p = sk(eval_expr(p, n));
        int fn = (int)mpf_get_si(n); mpf_clear(n);
        if (*p == ',') p = sk(p + 1);
        FileHandle *fh = fh_get(fn);
        if (!fh->fp) { basic_stderr("File not open: %d\n", fn); return -1; }
        int first = 1;
        while (*p) {
            if (!first) fprintf(fh->fp, ",");
            first = 0;
            if (is_str_token(p)) {
                char sbuf[1024]; p = sk(eval_str_expr(p, sbuf, sizeof sbuf));
                fprintf(fh->fp, "\"%s\"", sbuf);
            } else {
                mpf_t val; mpf_init2(val, g_prec);
                p = sk(eval_expr(p, val));
                /* Write integer if whole, else decimal */
                double d = mpf_get_d(val);
                if (d == floor(d) && fabs(d) < 1e15)
                    fprintf(fh->fp, "%.0f", d);
                else
                    fprintf(fh->fp, "%.7G", d);
                mpf_clear(val);
            }
            p = sk(p);
            if (*p == ',' || *p == ';') p = sk(p + 1);
            else break;
        }
        fprintf(fh->fp, "\n");
        return 0;
    }
    /* WRITE to screen */
    int first = 1;
    while (*p) {
        if (!first) display_putchar(',');
        first = 0;
        if (is_str_token(p)) {
            char sbuf[1024]; p = sk(eval_str_expr(p, sbuf, sizeof sbuf));
            display_putchar('"');
            display_print(sbuf);
            display_putchar('"');
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = sk(eval_expr(p, val));
            double d = mpf_get_d(val);
            char nbuf[64];
            if (d == floor(d) && fabs(d) < 1e15)
                snprintf(nbuf, sizeof nbuf, "%.0f", d);
            else
                snprintf(nbuf, sizeof nbuf, "%.7G", d);
            display_print(nbuf);
            mpf_clear(val);
        }
        p = sk(p);
        if (*p == ',' || *p == ';') p = sk(p + 1);
        else break;
    }
    display_newline();
    return 0;
}

/* ================================================================
 * TRON / TROFF  trace mode
 * ================================================================ */
static int cmd_tron(Interp *ip, char *args) {
    (void)ip; (void)args;
    g_tron = 1;
    return 0;
}
static int cmd_troff(Interp *ip, char *args) {
    (void)ip; (void)args;
    g_tron = 0;
    return 0;
}

/* ================================================================
 * ERROR n  raise a BASIC error (triggers ON ERROR handler)
 * ================================================================ */
static int cmd_error(Interp *ip, char *args) {
    mpf_t n; mpf_init2(n, g_prec);
    eval_expr(sk(args), n);
    g_err = (int)mpf_get_si(n);
    mpf_clear(n);
    /* If there's a handler, signal it; otherwise print and stop */
    if (g_error_handler[0]) {
        return -1;   /* run loop will jump to handler */
    }
    char buf[64];
    snprintf(buf, sizeof buf, "Error %d\n", g_err);
    display_print(buf);
    ip->running = 0;
    return 0;
}

/* ================================================================
 * LPRINT / LLIST  printer stubs (redirect to stdout)
 * ================================================================ */
static int cmd_print(Interp *ip, char *args);  /* forward */
static int cmd_debug(Interp *ip, char *args);  /* forward */
static int cmd_lprint(Interp *ip, char *args) {
    /* Treat exactly like PRINT */
    return cmd_print(ip, args);
}
static int cmd_llist(Interp *ip, char *args) {
    (void)ip; (void)args;
    /* Silently ignore  no printer */
    return 0;
}

/* ================================================================
 * OUT port, val / WAIT port, mask  hardware stubs
 * ================================================================ */
static int cmd_out(Interp *ip, char *args) {
    (void)ip;
    /* Consume arguments but do nothing */
    mpf_t a; mpf_init2(a, g_prec);
    char *p = sk(eval_expr(sk(args), a)); mpf_clear(a);
    if (*p == ',') { mpf_t b; mpf_init2(b, g_prec); eval_expr(sk(p+1), b); mpf_clear(b); }
    return 0;
}
static int cmd_wait(Interp *ip, char *args) {
    (void)ip; (void)args;
    return 0;
}
static int cmd_motor(Interp *ip, char *args) {
    (void)ip; (void)args;
    return 0;
}

/* ================================================================
 * VIEW PRINT [top TO bottom]  set text viewport (stub: just clear)
 * ================================================================ */
static int cmd_view_print(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    g_view_top = 1; g_view_bottom = 25;
    if (*p && *p != ':' && *p != '\'') {
        mpf_t a, b; mpf_init2(a, g_prec); mpf_init2(b, g_prec);
        p = sk(eval_expr(p, a));
        if (kw_match(p, "TO")) {
            p = sk(eval_expr(sk(p + 2), b));
            int t = (int)mpf_get_si(a), bt = (int)mpf_get_si(b);
            if (t >= 1 && bt >= t && bt <= 50) { g_view_top = t; g_view_bottom = bt; }
        }
        mpf_clears(a, b, NULL);
    }
    return 0;
}

/* ================================================================
 * PALETTE idx, ega_color  maps EGA palette slot to display colour.
 * gorilla.bas uses PALETTE 4, 0 style (EGA 6-bit color number).
 * ================================================================ */
static int cmd_palette(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    // Bare PALETTE with no arguments resets all slots to defaults
    if (!*p || *p == ':') {
#ifdef USE_SDL_WINDOW
        gfx_palette_reset_pub();
#endif
        return 0;
    }
    mpf_t v0, v1; mpf_init2(v0, g_prec); mpf_init2(v1, g_prec);
    p = sk(eval_expr(p, v0));
    if (*p == ',') p = sk(p + 1);
    p = sk(eval_expr(p, v1));
    int i = (int)mpf_get_si(v0);
    int r, g2, b;
    if (*p == ',') {
        /* 4-arg form: PALETTE idx, r, g, b */
        mpf_t vg, vb; mpf_init2(vg, g_prec); mpf_init2(vb, g_prec);
        p = sk(p + 1); p = sk(eval_expr(p, vg));
        if (*p == ',') p = sk(p + 1);
        eval_expr(p, vb);
        r  = (int)mpf_get_si(v1);
        g2 = (int)mpf_get_si(vg);
        b  = (int)mpf_get_si(vb);
        mpf_clears(vg, vb, NULL);
    } else {
        /* 2-arg form: PALETTE idx, ega6 */
        ega_to_rgb((int)mpf_get_si(v1), &r, &g2, &b);
    }
    mpf_clears(v0, v1, NULL);
#ifdef USE_SDL_WINDOW
    gfx_palette(i, r, g2, b);
#else
    felix_sendf("palette;%d;%d;%d;%d", i, r, g2, b);
#endif
    return 0;
}

/* ================================================================
 * POKE addr, val  stub
 * ================================================================ */
static int cmd_poke(Interp *ip, char *args) {
    (void)ip; (void)args; return 0;
}

/* ================================================================
 * END SUB / END FUNCTION / END SELECT  handled by their parent,
 * but we need them registered so they don't print "unknown".
 * END IF is similar.
 * ================================================================ */
static int cmd_end_sub(Interp *ip, char *args) {
    /* Only act as RETURN if we are inside a GOSUB/CALL frame.
     * During linear execution (falling through a SUB/FUNCTION body
     * that was never called) there is no frame -- skip silently. */
    for (int fi = g_ctrl_top - 1; fi >= 0; fi--)
        if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0)
            return cmd_return(ip, args);
    return 0;
}

/* ================================================================
 * CALL subname [args]  for now, treat as GOSUB to label
 * ================================================================ */
/* SUB parameters, as in QBasic. A variable given as an argument is passed
 * by reference: it gets the parameter's value back when the SUB returns.
 * An array passed as A() is the parameter array while the SUB runs (its
 * storage moves to the parameter and back). A parameter belongs to the
 * SUB: with procedure scopes (see scope_enter) it is one of the call's own
 * variables; in a program whose variables are all global, the variable of
 * that name is put back as it was when the SUB returns. Each entry belongs
 * to the call's frame on the control stack. */
typedef struct {
    int    frame;
    int    is_array;
    char   param[MAX_VARNAME];
    char   caller[MAX_VARNAME];     /* "" when passed by value */
    int    restore;                 /* global variables: put the parameter's variable back */
    int    saved_kind;              /* ... as it was: -1 when it didn't exist */
    mpf_t  saved_num;
    char  *saved_str;
} ByRef;
#define MAX_BYREF 256
static ByRef g_byref[MAX_BYREF];
static int   g_nbyref;

/* The scope each SUB call opened, by its frame. */
#define MAX_CALLS 256
static struct { int frame, scope; } g_calls[MAX_CALLS];
static int g_ncalls;

static void array_move(Var *to, Var *from) {
    var_free_arrays(to);
    to->kind = from->kind;
    memcpy(to->dim, from->dim, sizeof to->dim);
    to->ndim = from->ndim;
    to->arr_len = from->arr_len; to->arr_num = from->arr_num; to->arr_str = from->arr_str;
    from->arr_len = 0; from->arr_num = NULL; from->arr_str = NULL;
}

/* A TYPE array's elements are variables of their own (A.3.X): passing A()
 * passes them too, renamed into the parameter's name and scope and back. */
static void fields_move(char *from, int from_scope, char *to, int to_scope) {
    int fs = var_scope_for(from, from_scope), ts = var_scope_for(to, to_scope);
    size_t lf = strlen(from);
    if (fs == ts && strcasecmp(from, to) == 0) return;
    for (int i = 0; i < g_nvar; i++) {
        Var *v = &g_vars[i];
        if (!v->name || v->scope != fs) continue;
        if (strncasecmp(v->name, from, lf) != 0 || v->name[lf] != '.') continue;
        char buf[MAX_VARNAME * 2];
        snprintf(buf, sizeof buf, "%s%s", to, v->name + lf);
        free(v->name);
        v->name = bstrdup(buf);
        v->scope = ts;
    }
}

static void byref_drop(ByRef *b) {
    if (b->restore) { mpf_clear(b->saved_num); free(b->saved_str); b->saved_str = NULL; b->restore = 0; }
}

/* Put back the variable a parameter shadowed (global variables). */
static void byref_restore(ByRef *b, Var *pv) {
    if (b->saved_kind == VAR_STR) {
        if (pv->kind == VAR_STR) free(pv->str);
        pv->kind = VAR_STR; pv->str = b->saved_str; b->saved_str = NULL;
    } else if (b->saved_kind == VAR_NUM) {
        if (pv->kind == VAR_STR) { free(pv->str); pv->str = NULL; }
        pv->kind = VAR_NUM; mpf_set(pv->num, b->saved_num);
    } else if (var_is_str_name(b->param)) {
        if (pv->kind == VAR_STR) free(pv->str);
        pv->kind = VAR_STR; pv->str = str_dup((char *)"");
    } else if (pv->kind == VAR_NUM) {
        mpf_set_ui(pv->num, 0);
    }
}

/* Calls whose frames went without a return (GOTO out, RUN) are over. */
static void calls_prune(int top) {
    while (g_nbyref > 0 && g_byref[g_nbyref-1].frame >= top) byref_drop(&g_byref[--g_nbyref]);
    while (g_ncalls > 0 && g_calls[g_ncalls-1].frame >= top) scope_leave(g_calls[--g_ncalls].scope);
}

/* The SUB whose frame is `fi` returns: hand back what it was given, and
 * its variables go. (For a plain GOSUB there's nothing to do.) */
static void byref_return(int fi) {
    int scope = 0, has_call = 0;
    for (int k = g_ncalls - 1; k >= 0 && g_calls[k].frame >= fi; k--)
        if (g_calls[k].frame == fi) { scope = g_calls[k].scope; has_call = 1; }
    int caller_scope = scope > 0 ? scope - 1 : g_scope;
    while (g_nbyref > 0 && g_byref[g_nbyref-1].frame >= fi) {
        ByRef *b = &g_byref[--g_nbyref];
        if (b->frame != fi) { byref_drop(b); continue; }   /* unwound without returning */
        Var *pv = scope ? var_find_in(b->param, scope) : var_find(b->param);
        if (!pv) { byref_drop(b); continue; }
        if (b->is_array) {
            fields_move(b->param, scope ? scope : g_scope, b->caller, caller_scope);
            if (pv->kind != VAR_ARRAY_NUM && pv->kind != VAR_ARRAY_STR) continue;
            Var *cv = var_get_in(b->caller, caller_scope);
            pv = scope ? var_find_in(b->param, scope) : var_find(b->param);
            array_move(cv, pv);
            pv->kind = VAR_NUM; pv->ndim = 0;
            continue;
        }
        if (b->caller[0]) {
            Var *cv = var_get_in(b->caller, caller_scope);
            pv = scope ? var_find_in(b->param, scope) : var_find(b->param);
            if (var_is_str_name(b->caller)) {
                char *dup = str_dup((pv->kind == VAR_STR && pv->str) ? pv->str : (char *)"");
                if (cv->kind == VAR_STR) free(cv->str);
                cv->kind = VAR_STR; cv->str = dup;
            } else if (pv->kind == VAR_NUM && cv->kind == VAR_NUM) {
                mpf_set(cv->num, pv->num);
            }
        }
        if (b->restore) byref_restore(b, pv);
        byref_drop(b);
    }
    if (has_call)
        while (g_ncalls > 0 && g_calls[g_ncalls-1].frame >= fi) scope_leave(g_calls[--g_ncalls].scope);
}

/* The argument at cs, if it's a plain variable (or ARRAY()): its name. */
static int byref_arg(char *cs, char *name, int *is_array) {
    int n = 0;
    char *q = cs;
    *is_array = 0;
    if (!isalpha((unsigned char)*q)) return 0;
    while ((isalnum((unsigned char)*q) || *q == '_' || *q == '.') && n < MAX_VARNAME - 2) name[n++] = *q++;
    if (*q == '$' || *q == '!' || *q == '#' || *q == '%' || *q == '&') name[n++] = *q++;
    name[n] = '\0';
    q = sk(q);
    if (*q == '(') {
        char *inner = sk(q + 1);
        if (*inner != ')') return 0;           /* A(3): an element, by value */
        *is_array = 1;
        q = sk(inner + 1);
    }
    if (*q && *q != ',' && *q != ')') return 0;   /* an expression */
    return 1;
}

static int call_sub(Interp *ip, char *args, int bare);
static int cmd_call(Interp *ip, char *args) { return call_sub(ip, args, 0); }

/* One argument of a SUB call, evaluated in the caller before the call. */
typedef struct {
    char  pname[MAX_VARNAME];
    char  aname[MAX_VARNAME];   /* the variable passed, "" for an expression */
    int   is_array;
    int   is_str;
    mpf_t num;
    char *str;
} SubArg;

/* CALL Name(args), or bare Name args. Bare, "Name (X)" passes X by value,
 * as QBasic does: the parentheses make it an expression. */
static int call_sub(Interp *ip, char *args, int bare) {
    char *p = sk(args);
    char name[MAX_VARNAME]; int i = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && i < MAX_VARNAME - 1)
        name[i++] = *p++;
    name[i] = '\0';

    /* p now points past the name  skip optional parens/spaces to call-site args */
    p = sk(p);
    if (*p == '(' && !bare) p = sk(p + 1);

    /* Find the SUB definition line so we can read its parameter names.
     * The label points at the "SUB name ..." line itself. */
    int sub_idx = find_line_by_label(name);
    char *param_src = NULL;
    if (sub_idx >= 0) {
        char *sp = sk(g_lines[sub_idx].text);
        /* skip "SUB" or "FUNCTION" keyword */
        if (strncasecmp(sp, "SUB", 3) == 0)           sp = sk(sp + 3);
        else if (strncasecmp(sp, "FUNCTION", 8) == 0)  sp = sk(sp + 8);
        /* skip sub name */
        while (isalnum((unsigned char)*sp) || *sp == '_') sp++;
        sp = sk(sp);
        if (*sp == '(') sp = sk(sp + 1);
        param_src = sp;
    }

    calls_prune(g_ctrl_top);
    int frame = g_ctrl_top;           /* the frame cmd_gosub pushes below */

    /* 1. Evaluate the arguments, in the caller's scope. */
    static SubArg argv_[16];
    int nargs = 0;
    if (param_src && *param_src && *param_src != ')') {
        char *cs = p;          /* call-site arg pointer */
        char *ps = param_src;  /* param name pointer    */
        while (*ps && *ps != ')' && nargs < 16) {
            /* --- read one parameter name --- */
            ps = sk(ps);
            char pname[MAX_VARNAME]; int pi = 0;
            while ((isalnum((unsigned char)*ps) || *ps == '_' || *ps == '$'
                    || *ps == '!' || *ps == '#' || *ps == '%' || *ps == '&')
                   && pi < MAX_VARNAME - 1)
                pname[pi++] = *ps++;
            pname[pi] = '\0';
            /* skip optional () for array params */
            ps = sk(ps);
            if (*ps == '(') { ps++; while (*ps && *ps != ')') ps++; if (*ps == ')') ps++; }
            /* skip optional AS TYPE annotation */
            ps = sk(ps);
            if (strncasecmp(ps, "AS", 2) == 0 && isspace((unsigned char)ps[2])) {
                ps = sk(ps + 2);
                while (isalnum((unsigned char)*ps) || *ps == '_' || *ps == ' ') ps++;
            }

            /* --- evaluate one call-site argument --- */
            cs = sk(cs);
            if (!*cs || *cs == ')') break;

            SubArg *a = &argv_[nargs++];
            strcpy(a->pname, pname);
            a->aname[0] = 0;
            a->str = NULL;
            a->is_str = var_is_str_name(pname);
            char aname[MAX_VARNAME]; int is_arr = 0;
            if (byref_arg(cs, aname, &is_arr)) strcpy(a->aname, aname);
            a->is_array = is_arr;
            if (is_arr) {
                /* "ArrName()": no value, the array itself */
                char *look = cs;
                while (*look && *look != ')') look++;
                cs = sk(look + 1);
            } else if (a->is_str) {
                char sbuf[DEFAULT_BUFFER];
                cs = sk(eval_str_expr(cs, sbuf, sizeof sbuf));
                a->str = str_dup(sbuf);
            } else {
                mpf_init2(a->num, g_prec);
                cs = sk(eval_expr(cs, a->num));
            }
            cs = sk(cs);
            /* skip separating comma in both lists */
            if (*cs == ',') cs = sk(cs + 1);
            if (*ps == ',') ps = sk(ps + 1);
        }
    }

    if (sub_idx < 0) {
        for (int k = 0; k < nargs; k++) {
            if (argv_[k].is_array) continue;
            if (argv_[k].is_str) free(argv_[k].str); else mpf_clear(argv_[k].num);
        }
        return 0;
    }

    /* 2. The call's scope, and the parameters bound in it. */
    int caller_scope = g_scope;
    int scope = scope_enter();
    for (int k = 0; k < nargs; k++) {
        SubArg *a = &argv_[k];
        /* passing a variable to a parameter of the same name, with variables
         * global, is passing it to itself */
        int same = !scope && a->aname[0] && strcasecmp(a->aname, a->pname) == 0;
        if (a->is_array) {
            if (a->aname[0] && !same && g_nbyref < MAX_BYREF) {
                Var *cv = var_find_in(a->aname, caller_scope);
                Var *pv0 = var_find(a->pname);
                int c_arr = cv && (cv->kind == VAR_ARRAY_NUM || cv->kind == VAR_ARRAY_STR);
                int p_arr = pv0 && (pv0->kind == VAR_ARRAY_NUM || pv0->kind == VAR_ARRAY_STR);
                /* The caller's array, or (not dimensioned yet) the one the SUB DIMs */
                if (c_arr || !p_arr) {
                    Var *pv = var_get(a->pname);
                    cv = var_find_in(a->aname, caller_scope);
                    if (pv->kind == VAR_STR) { free(pv->str); pv->str = NULL; }
                    if (c_arr) array_move(pv, cv);
                    fields_move(a->aname, caller_scope, a->pname, g_scope);
                    ByRef *b = &g_byref[g_nbyref++];
                    memset(b, 0, sizeof *b);
                    b->frame = frame; b->is_array = 1;
                    strcpy(b->param, a->pname);
                    strcpy(b->caller, a->aname);
                }
            }
            continue;
        }
        if (!same && g_nbyref < MAX_BYREF && (scope == 0 || a->aname[0])) {
            ByRef *b = &g_byref[g_nbyref++];
            memset(b, 0, sizeof *b);
            b->frame = frame; b->is_array = 0;
            strcpy(b->param, a->pname);
            strcpy(b->caller, a->aname);
            if (scope == 0) {
                /* global variables: remember the one the parameter shadows */
                b->restore = 1;
                mpf_init2(b->saved_num, g_prec);
                b->saved_kind = -1;
                Var *pv0 = var_find(a->pname);
                if (pv0 && pv0->kind == VAR_STR) {
                    b->saved_kind = VAR_STR;
                    b->saved_str = str_dup(pv0->str ? pv0->str : (char *)"");
                } else if (pv0 && pv0->kind == VAR_NUM) {
                    b->saved_kind = VAR_NUM;
                    mpf_set(b->saved_num, pv0->num);
                }
            }
        }
        Var *v = var_get(a->pname);
        if (a->is_str) {
            if (v->kind == VAR_STR) free(v->str);
            v->kind = VAR_STR; v->str = a->str; a->str = NULL;
        } else {
            if (v->kind == VAR_NUM) { mpf_set(v->num, a->num); var_fix_int(v, v->num); }
            mpf_clear(a->num);
        }
    }
    if (g_ncalls < MAX_CALLS) {
        g_calls[g_ncalls].frame = frame;
        g_calls[g_ncalls].scope = scope;
        g_ncalls++;
    }

    int r = cmd_gosub(ip, name);
    if (r < 0) byref_return(frame);      /* the call didn't happen: give it all back */
    return r;
}

/* ================================================================
 * STATIC  variable declaration inside a SUB, treat like DIM
 * ================================================================ */
static int cmd_static(Interp *ip, char *args) {
    return cmd_dim(ip, args);
}


static int cmd_const(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        p = sk(p);
        if (*p == '=') p = sk(p + 1);
        /* string constant? */
        if (*p == '"') {
            char val[MAX_LINE_LEN]; int i = 0;
            p++;
            while (*p && *p != '"' && i < (int)sizeof(val) - 1) val[i++] = *p++;
            if (*p == '"') p++;
            val[i] = '\0';
            const_set(name, val, 1);
        } else {
            /* numeric  store the raw expression text for lazy eval */
            char *start = p;
            /* consume until comma or end (skipping parens) */
            int depth = 0;
            while (*p) {
                if (*p == '(') depth++;
                else if (*p == ')') { if (depth == 0) break; depth--; }
                else if (*p == ',' && depth == 0) break;
                p++;
            }
            char val[MAX_LINE_LEN];
            int len = (int)(p - start);
            if (len >= (int)sizeof(val)) len = (int)sizeof(val) - 1;
            memcpy(val, start, len);
            /* trim trailing whitespace */
            while (len > 0 && isspace((unsigned char)val[len - 1])) len--;
            val[len] = '\0';
            const_set(name, val, 0);
        }
        p = sk(p);
        if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

/* ================================================================
 * DIM  strip optional AS typename suffix before processing
 * ================================================================ */
static int cmd_dim(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    /* SHARED keyword  skip it, all our vars are already global */
    if (kw_match(p, "SHARED")) p = sk(p + 6);
    while (*p && *p != '\'') {
        char name[MAX_VARNAME];
        p = sk(read_varname(p, name));
        if (!name[0]) break;
        /* strip type sigil if present but not already in name */
        if ((*p == '&' || *p == '!' || *p == '#' || *p == '%') &&
            name[strlen(name)-1] != '$') p++;
        Var *v = var_find(name);
        if (!v) v = var_create(name);
        if (*p == '(') {
            p = sk(p + 1);

            #define PARSE_DIM(out_size) do { \
                mpf_t _a; mpf_init2(_a, g_prec); \
                p = sk(eval_expr(sk(p), _a)); \
                if (kw_match(p, "TO")) { \
                    mpf_clear(_a); \
                    p = sk(p + 2); \
                    mpf_t _b; mpf_init2(_b, g_prec); \
                    p = sk(eval_expr(sk(p), _b)); \
                    (out_size) = (int)mpf_get_si(_b) + 1 - g_option_base; \
                    mpf_clear(_b); \
                } else { \
                    (out_size) = (int)mpf_get_si(_a) + 1 - g_option_base; \
                    mpf_clear(_a); \
                } \
            } while(0)

            int dim1, dim2 = 1, ndim = 1;
            PARSE_DIM(dim1);
            if (*p == ',') { p = sk(p + 1); PARSE_DIM(dim2); ndim = 2; }
            #undef PARSE_DIM

            if (*p == ')') p++;
            if (dim1 < 1) dim1 = 1;
            if (dim2 < 1) dim2 = 1;
            int total = dim1 * dim2;
            if (total > MAX_ARRAY_SIZE) { basic_stderr("Array too large: %d\n", total); total = MAX_ARRAY_SIZE; }
            int is_str = var_is_str_name(name);
            /* A plain variable of the same name becomes the array. */
            if (v->kind == VAR_STR && v->str) { free(v->str); v->str = NULL; }
            v->kind = is_str ? VAR_ARRAY_STR : VAR_ARRAY_NUM;
            v->dim[0] = dim1; v->dim[1] = dim2; v->ndim = ndim;
            if (!var_alloc_array(v, total, is_str))
                basic_stderr("Out of memory for array %s(%d)\n", name, total);
        }
        p = sk(p);
        /* skip optional AS typename  e.g. "AS INTEGER", "AS PlayerData" */
        if (kw_match(p, "AS")) {
            p = sk(p + 2);
            /* read the type name */
            char type_name[MAX_VARNAME]; int tni = 0;
            while ((isalnum((unsigned char)*p) || *p == '_') && tni < MAX_VARNAME - 1)
                type_name[tni++] = (char)toupper((unsigned char)*p++);
            type_name[tni] = '\0';
            p = sk(p);
            if (*p == '*') { p = sk(p + 1); while (isdigit((unsigned char)*p)) p++; }
            p = sk(p);

            if (strcmp(type_name, "INTEGER") == 0 || strcmp(type_name, "LONG") == 0) v->is_int = 1;
            else if (strcmp(type_name, "SINGLE") == 0 || strcmp(type_name, "DOUBLE") == 0) v->is_int = 0;

            /* If it's a user-defined TYPE, create flat field variables */
            TypeDef *td = typedef_find(type_name);
            if (td) {
                Var *base_v = var_find(name);
                int is_arr = base_v && (base_v->kind == VAR_ARRAY_NUM ||
                                        base_v->kind == VAR_ARRAY_STR);
                for (int fi = 0; fi < td->nfields; fi++) {
                    char flatname[MAX_VARNAME];
                    snprintf(flatname, sizeof flatname, "%s.%s%s", name,
                             td->fields[fi].name, td->fields[fi].is_str ? "$" : "");
                    Var *fv = var_find(flatname);
                    if (!fv) fv = var_create(flatname);
                    fv->is_int = td->fields[fi].is_int;
                    if (!is_arr) continue;
                    /* An array of records: one array per field, shaped like
                     * the base array (see field_array). */
                    int fs = td->fields[fi].is_str;
                    if (fv->kind == VAR_STR) { free(fv->str); fv->str = NULL; }
                    fv->kind = fs ? VAR_ARRAY_STR : VAR_ARRAY_NUM;
                    fv->ndim = base_v->ndim;
                    fv->dim[0] = base_v->dim[0]; fv->dim[1] = base_v->dim[1];
                    if (!var_alloc_array(fv, base_v->arr_len, fs))
                        basic_stderr("Out of memory for array %s\n", flatname);
                }
            }
        }
        if (*p == ',') p = sk(p + 1);
    }
    return 0;
}

/* ================================================================
 * LET / implicit assignment
 * ================================================================ */
static int cmd_let(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);

    /* Check for struct field access: identifier optionally followed by (idx).field */
    {
        char *save = p;
        char flatname[MAX_VARNAME];
        FieldRef fr;
        char *after = parse_field_varname(p, flatname, &fr);
        if (after) {
            after = sk(after);
            if (*after == '=') {
                /* It's a field assignment */
                after = sk(after + 1);
                int fstr;
                Var *fa = fr.nidx ? field_array(fr.base, fr.field, &fstr) : NULL;
                if (fa && fstr) {
                    char sbuf[1024];
                    eval_str_expr(after, sbuf, sizeof sbuf);
                    char **e = arr_str_elem(fa, fr.i, fr.j);
                    free(*e); *e = str_dup(sbuf);
                    return 0;
                }
                if (fa) {
                    mpf_t val; mpf_init2(val, g_prec);
                    eval_expr(after, val);
                    var_fix_int(fa, val);
                    mpf_set(*arr_num_elem(fa, fr.i, fr.j), val);
                    mpf_clear(val);
                    return 0;
                }
                int is_str = (flatname[strlen(flatname)-1] == '$') ||
                             strrchr(flatname, '.') != NULL;
                /* Determine by trying string eval if it looks like a string */
                if (is_str_token(after)) {
                    char sbuf[1024];
                    eval_str_expr(after, sbuf, sizeof sbuf);
                    /* store as string  append $ sigil if not present */
                    char sname[MAX_VARNAME];
                    snprintf(sname, sizeof sname, "%s$", flatname);
                    Var *v = var_get(sname);
                    free(v->str); v->str = str_dup(sbuf);
                } else {
                    mpf_t val; mpf_init2(val, g_prec);
                    eval_expr(after, val);
                    Var *v = var_get(flatname);
                    mpf_set(v->num, val);
                    var_fix_int(v, v->num);
                    mpf_clear(val);
                }
                return 0;
            }
        }
        p = save;
    }

    char name[MAX_VARNAME];
    p = sk(read_varname(p, name));
    int arr_i = 0, arr_j = 1, is_arr = 0;
    if (*p == '(') {
        is_arr = 1; p = sk(p + 1);
        mpf_t i1; mpf_init2(i1, g_prec);
        p = sk(eval_expr(p, i1)); arr_i = (int)mpf_get_si(i1); mpf_clear(i1);
        if (*p == ',') { p=sk(p+1); mpf_t i2; mpf_init2(i2,g_prec); p=sk(eval_expr(p,i2)); arr_j=(int)mpf_get_si(i2); mpf_clear(i2); }
        if (*p == ')') p = sk(p + 1);
    }
    if (*p == '=') p = sk(p + 1);
    if (var_is_str_name(name)) {
        char sbuf[1024];
        eval_str_expr(sk(p), sbuf, sizeof sbuf);
        Var *v = var_get(name);
        if (is_arr && v->kind == VAR_ARRAY_STR) {
            char **slot = arr_str_elem(v, arr_i, arr_j);
            free(*slot); *slot = str_dup(sbuf);
        } else {
            free(v->str); v->str = str_dup(sbuf);
        }
    } else {
        mpf_t val; mpf_init2(val, g_prec);
        eval_expr(sk(p), val);
        if (is_arr) { Var *v = var_get(name); var_fix_int(v, val); mpf_set(*arr_num_elem(v, arr_i, arr_j), val); }
        else        { Var *v = var_get(name); mpf_set(v->num, val); var_fix_int(v, v->num); }
        mpf_clear(val);
    }
    return 0;
}

/* ================================================================
 * PRINT
 * ================================================================ */
static int cmd_print_file(Interp *ip, char *args);  /* forward */

static int cmd_print(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') return cmd_print_file(ip, args);

    if (kw_match(p, "USING")) {
        p = sk(p + 5);
        char fmt[1024];
        p = sk(eval_str_expr(p, fmt, sizeof fmt));   /* a literal or any string expression */
        if (*p == ';' || *p == ',') p = sk(p + 1);
        print_using_list(fmt, p);
        return 0;
    }

    int trailing_sep = 0;
    while (*p) {
        trailing_sep = 0;
        if (is_str_token(p)) {
            if (kw_match(p, "SPC")) {
                p = sk(p + 3); if (*p == '(') p++;
                mpf_t n; mpf_init2(n, g_prec); p = eval_expr(sk(p), n);
                display_spc((int)mpf_get_si(n)); mpf_clear(n);
                p = sk(p); if (*p == ')') p = sk(p + 1);
            } else if (kw_match(p, "TAB")) {
                p = sk(p + 3); if (*p == '(') p++;
                mpf_t n; mpf_init2(n, g_prec); p = eval_expr(sk(p), n);
                int col = (int)mpf_get_si(n) - 1; mpf_clear(n);
                p = sk(p); if (*p == ')') p = sk(p + 1);
                if (col > 0) display_spc(col);
            } else {
                char sbuf[1024];
                p = eval_str_expr(p, sbuf, sizeof sbuf);
                display_print(sbuf);
            }
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = eval_expr(p, val);
            double d = mpf_get_d(val);
            if (d == floor(d) && fabs(d) < 1e15) {
                if (d >= 0) printf(" %.0f ", d);
                else        printf("%.0f ", d);
            } else {
                char buf[64];
                snprintf(buf, sizeof buf, "%.7G", d);
                if (strchr(buf, '.') && !strchr(buf, 'E')) {
                    char *end = buf + strlen(buf) - 1;
                    while (*end == '0') *end-- = '\0';
                    if (*end == '.') *end = '\0';
                }
                if (d >= 0) printf(" %s ", buf);
                else        printf("%s ", buf);
            }
            mpf_clear(val);
        }
        p = sk(p);
        if (*p == ';') { trailing_sep = 1; p = sk(p + 1); }
        else if (*p == ',') { display_putchar('\t'); trailing_sep = 1; p = sk(p + 1); }
        else break;
    }
    if (!trailing_sep) display_newline();
    return 0;
}

static int cmd_debug(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);

    int trailing_sep = 0;
    while (*p) {
        trailing_sep = 0;
        if (is_str_token(p)) {
            if (kw_match(p, "SPC")) {
                p = sk(p + 3); if (*p == '(') p++;
                mpf_t n; mpf_init2(n, g_prec); p = eval_expr(sk(p), n);
                int spaces = (int)mpf_get_si(n); mpf_clear(n);
                p = sk(p); if (*p == ')') p = sk(p + 1);
                for (int i = 0; i < spaces; i++) SDL_Log(" ");
            } else if (kw_match(p, "TAB")) {
                p = sk(p + 3); if (*p == '(') p++;
                mpf_t n; mpf_init2(n, g_prec); p = eval_expr(sk(p), n);
                p = sk(p); if (*p == ')') p = sk(p + 1);
                mpf_clear(n);
            } else {
                char sbuf[1024];
                p = eval_str_expr(p, sbuf, sizeof sbuf);
                SDL_Log("%s", sbuf);
            }
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = eval_expr(p, val);
            double d = mpf_get_d(val);
            if (d == floor(d) && fabs(d) < 1e15) {
                if (d >= 0) SDL_Log(" %.0f ", d);
                else        SDL_Log("%.0f ", d);
            } else {
                char buf[64];
                snprintf(buf, sizeof buf, "%.7G", d);
                if (strchr(buf, '.') && !strchr(buf, 'E')) {
                    char *end = buf + strlen(buf) - 1;
                    while (*end == '0') *end-- = '\0';
                    if (*end == '.') *end = '\0';
                }
                if (d >= 0) SDL_Log(" %s ", buf);
                else        SDL_Log("%s ", buf);
            }
            mpf_clear(val);
        }
        p = sk(p);
        if (*p == ';') { trailing_sep = 1; p = sk(p + 1); }
        else if (*p == ',') { SDL_Log("\t"); trailing_sep = 1; p = sk(p + 1); }
        else break;
    }
    if (!trailing_sep) SDL_Log("\n");
    return 0;
}

/* ================================================================
 * LINE INPUT / INPUT
 * ================================================================ */
static int cmd_line_input_file(Interp *ip, char *args);  /* forward */
static int cmd_input_file(Interp *ip, char *args);       /* forward */

static int cmd_line_input(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') return cmd_line_input_file(ip, args);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') display_putchar(*p++);
        if (*p == '"') p++;
    }
    p = sk(p); if (*p == ';' || *p == ',') p = sk(p + 1);
    char name[MAX_VARNAME];
    read_varname(p, name);
    char linebuf[DEFAULT_BUFFER];
    display_cursor(1);
    display_getline(linebuf, sizeof linebuf);
#ifndef USE_SDL_WINDOW
    display_newline();
#endif
    Var *v = var_get(name);
    free(v->str); v->str = str_dup(linebuf);
    return 0;
}

static int cmd_input(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') return cmd_input_file(ip, args);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') display_putchar(*p++);
        if (*p == '"') p++;
        p = sk(p);
        /* semicolon: print "? " after prompt (QBasic default)
         * comma: no "? " (suppressed)
         * neither: treat as semicolon */
        if (*p == ',') {
            p = sk(p + 1);
            /* no ? */
        } else {
            if (*p == ';') p = sk(p + 1);
            display_print("? ");
        }
    } else {
        display_print("? ");
    }
    char linebuf[DEFAULT_BUFFER];
    display_cursor(1);
    display_getline(linebuf, sizeof linebuf);
#ifndef USE_SDL_WINDOW
    display_newline();
#endif
    char *tok = linebuf;
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        char *comma = strchr(tok, ',');
        char val_str[DEFAULT_BUFFER];
        if (comma) {
            size_t len = (size_t)(comma - tok);
            if (len >= sizeof val_str) len = sizeof val_str - 1;
            memcpy(val_str, tok, len); val_str[len] = '\0';
            tok = comma + 1;
        } else {
            strncpy(val_str, tok, sizeof val_str - 1);
            val_str[sizeof val_str - 1] = '\0';
            tok += strlen(tok);
        }
        char *v_start = val_str;
        while (isspace((unsigned char)*v_start)) v_start++;
        char *v_end = v_start + strlen(v_start);
        while (v_end > v_start && isspace((unsigned char)v_end[-1])) *--v_end = '\0';
        Var *v = var_get(name);
        if (var_is_str_name(name)) { free(v->str); v->str = str_dup(v_start); }
        else                       { mpf_set_d(v->num, atof(v_start)); var_fix_int(v, v->num); }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

/* ================================================================
 * File I/O commands
 * ================================================================ */
static int cmd_open(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    char filename[DEFAULT_BUFFER]; int fi = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && fi < (int)sizeof(filename) - 1) filename[fi++] = *p++;
        if (*p == '"') p++;
    }
    filename[fi] = '\0';
    tilde_expand(filename, sizeof(filename));
    p = sk(p);
    char mode_ch = 'O';
    if (kw_match(p, "FOR")) {
        p = sk(p + 3);
        if      (kw_match(p, "INPUT"))  { mode_ch = 'I'; p = sk(p + 5); }
        else if (kw_match(p, "OUTPUT")) { mode_ch = 'O'; p = sk(p + 6); }
        else if (kw_match(p, "APPEND")) { mode_ch = 'A'; p = sk(p + 6); }
    }
    if (kw_match(p, "AS")) p = sk(p + 2);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = eval_expr(p, num);
    int n = (int)mpf_get_si(num); mpf_clear(num);
    FileHandle *fh = fh_get(n);
    if (fh->fp) { fclose(fh->fp); fh->fp = NULL; }
    fh->fp = fopen(filename, (mode_ch == 'I') ? "r" : (mode_ch == 'A') ? "a" : "w");
    if (!fh->fp) { perror(filename); return 0; }
    fh->mode = mode_ch;
    return 0;
}

static int cmd_close(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!*p) {
        for (int i = 1; i <= MAX_FILE_HANDLES; i++)
            if (g_files[i].fp) { fclose(g_files[i].fp); g_files[i].fp = NULL; g_files[i].mode = 0; }
        return 0;
    }
    while (*p) {
        if (*p == '#') p = sk(p + 1);
        mpf_t num; mpf_init2(num, g_prec);
        p = sk(eval_expr(p, num));
        int n = (int)mpf_get_si(num); mpf_clear(num);
        FileHandle *fh = fh_get(n);
        if (fh->fp) { fclose(fh->fp); fh->fp = NULL; fh->mode = 0; }
        if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

static int cmd_input_file(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode != 'I') { basic_stderr("File #%d not open for input\n", n); return 0; }
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        char linebuf[DEFAULT_BUFFER];
        if (!fgets(linebuf, sizeof linebuf, fh->fp)) linebuf[0] = '\0';
        linebuf[strcspn(linebuf, "\r\n")] = '\0';
        char *val = linebuf;
        while (isspace((unsigned char)*val)) val++;
        if (*val == '"') { val++; char *q = strchr(val, '"'); if (q) *q = '\0'; }
        Var *v = var_get(name);
        if (var_is_str_name(name)) { free(v->str); v->str = str_dup(val); }
        else                       { mpf_set_d(v->num, atof(val)); var_fix_int(v, v->num); }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

static int cmd_print_file(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode == 'I') { basic_stderr("File #%d not open for output\n", n); return 0; }
    int trailing = 0;
    while (*p) {
        trailing = 0;
        if (is_str_token(p)) {
            char sbuf[1024]; p = eval_str_expr(p, sbuf, sizeof sbuf); fputs(sbuf, fh->fp);
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = eval_expr(p, val);
            double d = mpf_get_d(val); mpf_clear(val);
            if (d == floor(d) && fabs(d) < 1e15)
                fprintf(fh->fp, d >= 0 ? " %.0f " : "%.0f ", d);
            else
                fprintf(fh->fp, d >= 0 ? " %g " : "%g ", d);
        }
        p = sk(p);
        if      (*p == ';') { trailing = 1; p = sk(p + 1); }
        else if (*p == ',') { fputc('\t', fh->fp); trailing = 1; p = sk(p + 1); }
        else break;
    }
    if (!trailing) fputc('\n', fh->fp);
    return 0;
}

static int cmd_line_input_file(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode != 'I') { basic_stderr("File #%d not open for input\n", n); return 0; }
    char name[MAX_VARNAME];
    read_varname(sk(p), name);
    char linebuf[DEFAULT_BUFFER];
    if (!fgets(linebuf, sizeof linebuf, fh->fp)) linebuf[0] = '\0';
    linebuf[strcspn(linebuf, "\r\n")] = '\0';
    Var *v = var_get(name);
    free(v->str); v->str = str_dup(linebuf);
    return 0;
}

/* ================================================================
 * GET/PUT graphics  Felix sprite capture/blit.
 * GET (x1,y1)-(x2,y2), array_var
 * PUT (x,y), array_var [, PSET|XOR]
 * ================================================================ */
static int cmd_get_graphics(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x1, y1, x2, y2;
    p = parse_xy(p, &x1, &y1);
    if (*p == '-') p = sk(p + 1);
    p = parse_xy(p, &x2, &y2);
    if (*p == ',') p = sk(p + 1);
    char vname[MAX_VARNAME];
    read_varname(sk(p), vname);
    Var *v = var_get(vname);
    
    /* Get unique sprite ID for this array variable */
    int id = sprite_id_for(v);
    
#ifdef BASIC_DEBUG_GFX
    basic_stderr("GET: var='%s' id=%d coords=(%d,%d)-(%d,%d)\n", 
                 vname, id, (int)x1, (int)y1, (int)x2, (int)y2);
#endif
    
#ifdef USE_SDL_WINDOW
    /* CRITICAL: Flush any pending SDL render so pixel buffer is current */
    ::gfx_sdl_render();
    
    /* Now capture sprite into the registry under this ID */
    gfx_get(id, (int)x1, (int)y1, (int)x2, (int)y2);
#ifdef BASIC_DEBUG_GFX
    basic_stderr("  -> gfx_get() called for sprite id=%d\n", id);
#endif
#else
    /* OSC 666 path */
    felix_drawf("get;%d;%d;%d;%d;%d", id,
                (int)x1, (int)y1, (int)x2, (int)y2);
#endif
    
    return 0;
}

static int cmd_put_graphics(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = parse_xy(p, &x, &y);
    if (*p == ',') p = sk(p + 1);
    char vname[MAX_VARNAME];
    p = sk(read_varname(sk(p), vname));
    Var *v = var_get(vname);
    /* The array may be written Name() or Name(start) */
    if (*p == '(') {
        int depth = 1;
        for (p++; *p && depth > 0; p++) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
        }
        p = sk(p);
    }
    /* Action: PSET, PRESET, AND, OR or XOR; QBasic's default is XOR */
    const char *mode = "xor";
    int put_mode = 1;
    if (*p == ',') {
        p = sk(p + 1);
        if (kw_match(p, "PSET"))        { mode = "pset";   put_mode = 0; }
        else if (kw_match(p, "PRESET")) { mode = "preset"; put_mode = 2; }
        else if (kw_match(p, "AND"))    { mode = "and";    put_mode = 3; }
        else if (kw_match(p, "OR"))     { mode = "or";     put_mode = 4; }
    }

    /* Get the unique sprite ID assigned to this array variable */
    int id = sprite_id_for(v);

#ifdef USE_SDL_WINDOW
    if (gfx_sprite_exists(id)) {
        gfx_put(id, (int)x, (int)y, put_mode);
    } else if (v->kind == VAR_ARRAY_NUM && v->arr_num && v->arr_len > 0) {
        /* An array filled in by the program (READ from DATA, as Gorillas'
         * bananas are): a GET image in the PC's own format. */
        int n = v->arr_len;
        int *raw = (int *)malloc((size_t)n * sizeof(int));
        if (raw) {
            for (int i = 0; i < n; i++) {
                double d = mpf_get_d(v->arr_num[i]);
                raw[i] = (int)(uint32_t)(long long)d;
            }
            gfx_put_array(raw, n, (int)x, (int)y, put_mode);
            free(raw);
        }
    }
#ifdef BASIC_DEBUG_GFX
    basic_stderr("PUT: %s (id=%d) at (%d,%d), mode=%s\n", vname, id, (int)x, (int)y, mode);
#endif
#else
    /* OSC path */
    felix_drawf("put;%d;%d;%d;%s", id, (int)x, (int)y, mode);
#endif
    
    return 0;
}

static int cmd_draw(Interp *ip, char *args) {
    (void)ip; (void)args; return 0;  /* DRAW string mini-language  not needed for gorilla */
}

/* ================================================================
 * CIRCLE (x, y), r [, color [, start_angle, end_angle [, aspect]]]
 * ================================================================ */
static int cmd_circle(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = sk(parse_xy(p, &x, &y));
    if (*p == ',') p = sk(p + 1);
    mpf_t mr; mpf_init2(mr, g_prec);
    p = sk(eval_expr(p, mr));
    double r = mpf_get_d(mr); mpf_clear(mr);
    long color_raw = 15;
    if (*p == ',') {
        p = sk(p + 1);
        if (*p != ',' && *p != '\00' && *p != ':') {
            mpf_t mc; mpf_init2(mc, g_prec);
            p = sk(eval_expr(p, mc));
            color_raw = mpf_get_si(mc); mpf_clear(mc);
        }
    }
    int color = color_resolve(color_raw);
    /* Parse optional start_angle, end_angle, aspect */
    double start_angle = 0.0, end_angle = 0.0, aspect = -1.0;
    int has_arc = 0, filled = 0;
    int arg_n = 0;
    while (*p == ',') {
        p = sk(p + 1);
        if (*p == 'F' || *p == 'f') { filled = 1; p++; continue; }
        if (*p == ',' || *p == '\0' || *p == ':') continue;  /* skip empty args */
        mpf_t tmp; mpf_init2(tmp, g_prec);
        p = sk(eval_expr(p, tmp));
        p = sk(p);  /* Skip whitespace after the expression */
        double val = mpf_get_d(tmp); mpf_clear(tmp);
        if (arg_n == 0)      { start_angle = val; has_arc = 1; }
        else if (arg_n == 1) { end_angle   = val; has_arc = 1; }
        else if (arg_n == 2) aspect = val;
        arg_n++;
    }
#ifdef USE_SDL_WINDOW
    /* QBasic's aspect ratio: y radius / x radius. By default the screen's
     * own (4/3 x height/width), so circles come out round on the 4:3
     * screen every mode is shown on. Below 1, r is the x radius; above,
     * the y radius. */
    if (aspect <= 0 && gfx_width() > 0)
        aspect = 4.0 / 3.0 * gfx_height() / gfx_width();
    if (aspect <= 0) aspect = 1.0;
    double rxd = r, ryd = r;
    if (aspect < 1.0) ryd = r * aspect; else rxd = r / aspect;
    int rx = (int)(rxd + 0.5), ry = (int)(ryd + 0.5);
    if (filled) {
        gfx_ellipse((int)x, (int)y, rx, ry, color);
        gfx_paint((int)x, (int)y, color, color);
    } else if (has_arc) {
        gfx_ellipse_arc((int)x, (int)y, rx, ry, start_angle, end_angle, color);
    } else {
        gfx_ellipse((int)x, (int)y, rx, ry, color);
    }
#else
    felix_drawf("circle;%d;%d;%d;%d",
                (int)x, (int)y, (int)(r + 0.5), color);
#endif
    return 0;
}

/* ================================================================
 * LINE [(x1,y1)]-(x2,y2), color [, B[F]]
 * ================================================================ */
static int cmd_line_gfx(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    /* Default start point is the Current Graphics Position */
    double x1 = g_gfx_x, y1 = g_gfx_y, x2, y2;

    /* optional explicit start point */
    if (*p == '(') {
        p = sk(parse_xy(p, &x1, &y1));
    }
    if (*p == '-') p = sk(p + 1);
    p = sk(parse_xy(p, &x2, &y2));

    int color = 15;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        p = sk(eval_expr(p, mc));
        color = (int)mpf_get_si(mc); mpf_clear(mc);
    }

    char *suffix = "";
    p = sk(p);
    if (*p == ',') {
        p = sk(p + 1);
        if (kw_match(p, "BF")) suffix = ";BF";
        else if (*p == 'B' || *p == 'b') suffix = ";B";
    }

    /* Update Current Graphics Position to endpoint */
    g_gfx_x = x2; g_gfx_y = y2;

#ifdef USE_SDL_WINDOW
    if      (strcmp(suffix, ";BF") == 0) gfx_boxfill((int)x1,(int)y1,(int)x2,(int)y2, color);
    else if (strcmp(suffix, ";B")  == 0) gfx_box    ((int)x1,(int)y1,(int)x2,(int)y2, color);
    else                                 gfx_line   ((int)x1,(int)y1,(int)x2,(int)y2, color);
#else
    felix_drawf("line;%d;%d;%d;%d;%d%s",
                (int)x1, (int)y1, (int)x2, (int)y2, color, suffix);
#endif
    return 0;
}

/* ================================================================
 * PAINT (x, y), fill_color [, border_color]
 * ================================================================ */
static int cmd_paint(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = sk(parse_xy(p, &x, &y));
    int fill = 15, border = -1;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        p = sk(eval_expr(p, mc));
        fill = (int)mpf_get_si(mc); mpf_clear(mc);
    }
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        eval_expr(p, mc);
        border = (int)mpf_get_si(mc); mpf_clear(mc);
    }
    int bc = (border >= 0) ? border : fill;
#ifdef USE_SDL_WINDOW
    gfx_paint((int)x, (int)y, fill, bc);
#else
    if (border >= 0)
        felix_drawf("paint;%d;%d;%d;%d", (int)x, (int)y, fill, border);
    else
        felix_drawf("paint;%d;%d;%d", (int)x, (int)y, fill);
#endif
    return 0;
}

/* ================================================================
 * PSET (x, y) [, color]
 * ================================================================ */
static int cmd_pset(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = sk(parse_xy(p, &x, &y));
    int color = 15;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        eval_expr(p, mc);
        color = (int)mpf_get_si(mc); mpf_clear(mc);
    }
    g_gfx_x = x; g_gfx_y = y;  /* update Current Graphics Position */
#ifdef USE_SDL_WINDOW
    gfx_pset((int)x, (int)y, color);
#else
    felix_drawf("pset;%d;%d;%d", (int)x, (int)y, color);
#endif
    return 0;
}

/* ================================================================
 * PRESET (x, y) [, color]    like PSET but default color is 0 (background)
 * ================================================================ */
static int cmd_preset(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = sk(parse_xy(p, &x, &y));
    int color = 0;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        eval_expr(p, mc);
        color = (int)mpf_get_si(mc); mpf_clear(mc);
    }
    g_gfx_x = x; g_gfx_y = y;  /* update Current Graphics Position */
#ifdef USE_SDL_WINDOW
    gfx_pset((int)x, (int)y, color);
#else
    felix_drawf("pset;%d;%d;%d", (int)x, (int)y, color);
#endif
    return 0;
}

/* ================================================================
 * Utility: build a flat variable name for struct field access.
 * "PDat(2).PNam"  "PDAT.2.PNAM"
 * "Settings.UseSound"  "SETTINGS.USESOUND"
 * Result written into out (must be MAX_VARNAME bytes).
 * Returns pointer past the parsed text, or NULL on failure.
 * ================================================================ */
static char *parse_field_varname(char *p, char *out, FieldRef *fr) {
    /* read base name */
    char base[MAX_VARNAME]; int bi = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && bi < MAX_VARNAME - 1)
        base[bi++] = (char)toupper((unsigned char)*p++);
    base[bi] = '\0';
    if (!bi) return NULL;

    fr->nidx = 0;
    /* optional array index */
    char idx_str[32] = "";
    if (*p == '(') {
        p = sk(p + 1);
        mpf_t v; mpf_init2(v, g_prec);
        p = sk(eval_expr(p, v));
        int idx1 = (int)mpf_get_si(v);
        mpf_clear(v);
        snprintf(idx_str, sizeof idx_str, "%d", idx1);
        fr->nidx = 1; fr->i = idx1; fr->j = g_option_base;
        if (*p == ',') {
            p = sk(p + 1);
            mpf_t v2; mpf_init2(v2, g_prec);
            p = sk(eval_expr(p, v2));
            int idx2 = (int)mpf_get_si(v2);
            mpf_clear(v2);
            fr->nidx = 2; fr->j = idx2;
            char tmp2[16]; snprintf(tmp2, sizeof tmp2, ",%d", idx2);
            strncat(idx_str, tmp2, sizeof idx_str - strlen(idx_str) - 1);
        }
        if (*p == ')') p++;
    }
    p = sk(p);
    if (*p != '.') return NULL;
    p = sk(p + 1);

    /* read field name */
    char field[MAX_VARNAME]; int fi = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && fi < MAX_VARNAME - 1)
        field[fi++] = (char)toupper((unsigned char)*p++);
    field[fi] = '\0';
    if (!fi) return NULL;

    strcpy(fr->base, base); strcpy(fr->field, field);
    /* build flat name: BASE.IDX.FIELD or BASE.FIELD */
    if (idx_str[0])
        snprintf(out, MAX_VARNAME, "%s.%s.%s", base, idx_str, field);
    else
        snprintf(out, MAX_VARNAME, "%s.%s", base, field);

    return p;
}
static int cmd_for(Interp *ip, char *args) {
    char *p = sk(args);
    char vname[MAX_VARNAME];
    p = sk(read_varname(p, vname));
    if (*p == '=') p = sk(p + 1);
    mpf_t start, limit, step;
    mpf_init2(start, g_prec); mpf_init2(limit, g_prec); mpf_init2(step, g_prec);
    mpf_set_ui(step, 1);
    p = sk(eval_expr(p, start));
    if (strncasecmp(p, "TO", 2) == 0) p = sk(p + 2);
    p = sk(eval_expr(p, limit));
    if (strncasecmp(p, "STEP", 4) == 0) { p = sk(p + 4); eval_expr(p, step); }
    Var *v = var_get(vname); mpf_set(v->num, start); var_fix_int(v, v->num);

    if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stacktrace("Stack overflow"); return -1; }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strncpy(f->varname, vname, MAX_VARNAME - 1);
    mpf_init2(f->limit, g_prec); mpf_set(f->limit, limit);
    mpf_init2(f->step,  g_prec); mpf_set(f->step,  step);
    f->line_idx = ip->pc;
    mpf_clears(start, limit, step, NULL);
    return 0;
}

static int cmd_next(Interp *ip, char *args) {
    char *p = sk(args);
    char vname[MAX_VARNAME] = "";
    if (isalpha((unsigned char)*p)) read_varname(p, vname);
    int fi = g_ctrl_top - 1;
    if (*vname) {
        for (fi = g_ctrl_top - 1; fi >= 0; fi--)
            if (strcasecmp(g_ctrl[fi].varname, vname) == 0) break;
        if (fi < 0) { basic_stderr("NEXT without FOR: %s\n", vname); return -1 ; }
    }
    if (fi < 0) { basic_stderr("NEXT without FOR\n"); return -1; }
    CtrlFrame *f = &g_ctrl[fi];
    Var *cv = var_get(f->varname);
    mpf_add(cv->num, cv->num, f->step);
    int done = (mpf_sgn(f->step) > 0)
             ? (mpf_cmp(cv->num, f->limit) > 0)
             : (mpf_cmp(cv->num, f->limit) < 0);
    if (done) { mpf_clear(f->limit); mpf_clear(f->step); g_ctrl_top = fi; return 0; }

#ifdef USE_SDL_WINDOW
    /* Pace FOR loops to ~1 million iterations/second (4MHz PC equivalent).
     * Check wall clock every 1000 iterations and sleep if running ahead. */
    {
        static int    s_for_count = 0;
        static Uint32 s_for_t0    = 0;
        static long   s_for_total = 0;
        if (++s_for_count >= 1000) {
            s_for_count = 0;
            s_for_total += 1000;
            if (s_for_t0 == 0 || s_for_total > 60000000L) {
                /* First call or reset every ~60 seconds to prevent overflow/drift */
                s_for_t0    = SDL_GetTicks();
                s_for_total = 1000;
            }
            ::gfx_sdl_pump();
            if (!basic_paced()) ::gfx_sdl_render();
            /* At 1M iters/sec, 1000 iters should take 1ms */
            Uint32 target_ms = (Uint32)(s_for_total / 1000);
            Uint32 elapsed   = SDL_GetTicks() - s_for_t0;
            if (target_ms > elapsed + 1)
                SDL_Delay(target_ms - elapsed - 1);
        }
    }
#endif

    ip->pc = f->line_idx + 1; return 1;
}

int cmd_goto(Interp *ip, char *args) {
    char *p = sk(args);
    int idx;
    if (isdigit((unsigned char)*p)) {
        idx = find_line_idx(atoi(p));
    } else {
        /* label-based jump: read the identifier and look it up */
        char lname[MAX_VARNAME]; int i = 0;
        while ((isalnum((unsigned char)*p) || *p == '_') && i < MAX_VARNAME - 1)
            lname[i++] = *p++;
        lname[i] = '\0';
        idx = find_line_by_label(lname);
    }
    if (idx < 0) { basic_stderr("GOTO: target not found: %s\n", sk(args)); return -1; }
    ip->pc = idx; return 1;
}

int cmd_gosub(Interp *ip, char *args) {
    if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stacktrace("Stack overflow"); return -1; }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strcpy(f->varname, "\x01" "GOSUB");
    f->line_idx = ip->pc + 1;
    mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
    mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
    return cmd_goto(ip, args);
}

static int cmd_return(Interp *ip, char *args) {
    (void)args;
    for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
        if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0) {
            byref_return(fi);
            ip->pc = g_ctrl[fi].line_idx;
            mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
            g_ctrl_top = fi; return 1;
        }
    }
    basic_stderr("RETURN without GOSUB\n"); return -1;
}

/* ================================================================
 * IF / THEN / ELSE
 * ================================================================ */
static int eval_one_cmp(char **pp) {
    char *p = sk(*pp);
    int cmp = 0;

    if (*p == '(') {
        p++;
        if (is_str_token(p)) {
            char lhs[1024], rhs[1024];
            p = sk(eval_str_or_inkey(p, lhs, sizeof lhs));
            char op2[3] = {p[0], p[0] ? p[1] : '\0', '\0'}; int oplen = 2;
            if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
                !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
            else { op2[1] = '\0'; oplen = 1; }
            p = sk(eval_str_or_inkey(sk(p + oplen), rhs, sizeof rhs));
            int c = strcmp(lhs, rhs);
            if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
            else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
            else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
            else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
            else cmp=(c==0);
        } else {
            mpf_t lhs; mpf_init2(lhs, g_prec);
            p = sk(eval_expr(p, lhs));
            char op2[3] = {p[0], p[0] ? p[1] : '\0', '\0'}; int oplen = 2;
            if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
                !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
            else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op2[1]='\0'; oplen=1; }
            else { cmp=(mpf_sgn(lhs)!=0); mpf_clear(lhs); goto closeparen; }
            mpf_t rhs; mpf_init2(rhs, g_prec);
            p = sk(eval_expr(sk(p + oplen), rhs));
            int c = mpf_cmp(lhs, rhs);
            if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
            else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
            else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
            else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
            else cmp=(c==0);
            mpf_clears(lhs, rhs, NULL);
            while (kw_match(p,"AND") || kw_match(p,"OR")) {
                int is_and = kw_match(p,"AND");
                p = sk(p + (is_and ? 3 : 2));
                mpf_t lhs2; mpf_init2(lhs2, g_prec);
                p = sk(eval_expr(p, lhs2));
                char op3[3] = {p[0], p[0] ? p[1] : '\0', '\0'}; int ol3 = 2;
                if (!strcmp(op3,"<>")||!strcmp(op3,"><")||!strcmp(op3,"<=")||
                    !strcmp(op3,"=<")||!strcmp(op3,">=")||!strcmp(op3,"=>")) ;
                else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op3[1]='\0'; ol3=1; }
                else { int r2=(mpf_sgn(lhs2)!=0); mpf_clear(lhs2);
                       cmp=is_and?(cmp&&r2):(cmp||r2); continue; }
                mpf_t rhs2; mpf_init2(rhs2, g_prec);
                p = sk(eval_expr(sk(p + ol3), rhs2));
                int c2 = mpf_cmp(lhs2, rhs2), cmp2;
                if      (!strcmp(op3,"<>")||!strcmp(op3,"><")) cmp2=(c2!=0);
                else if (!strcmp(op3,"<=")||!strcmp(op3,"=<")) cmp2=(c2<=0);
                else if (!strcmp(op3,">=")||!strcmp(op3,"=>")) cmp2=(c2>=0);
                else if (op3[0]=='<') cmp2=(c2<0); else if (op3[0]=='>') cmp2=(c2>0);
                else cmp2=(c2==0);
                mpf_clears(lhs2, rhs2, NULL);
                cmp = is_and ? (cmp && cmp2) : (cmp || cmp2);
            }
        }
        closeparen:
        p = sk(p); if (*p == ')') p = sk(p + 1);
        *pp = p;
        return cmp;
    }

    if (is_str_token(p)) {
        char lhs[1024], rhs[1024];
        p = sk(eval_str_or_inkey(p, lhs, sizeof lhs));
        char op2[3] = {p[0], p[0] ? p[1] : '\0', '\0'}; int oplen = 2;
        if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
            !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
        else { op2[1]='\0'; oplen=1; }
        p = sk(eval_str_or_inkey(sk(p + oplen), rhs, sizeof rhs));
        int c = strcmp(lhs, rhs);
        if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
        else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
        else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
        else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
        else cmp=(c==0);
    } else {
        mpf_t lhs; mpf_init2(lhs, g_prec);
        p = sk(eval_expr(p, lhs));
        char op2[3] = {p[0], p[0] ? p[1] : '\0', '\0'}; int oplen = 2;
        if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
            !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
        else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op2[1]='\0'; oplen=1; }
        else { 
            double dbgv = mpf_get_d(lhs);
            int dbgcmp = (mpf_sgn(lhs)!=0);
            //basic_stderr("[eval_one_cmp] no-op path: lhs=%.4f -> cmp=%d, rem='%.30s'\n", dbgv, dbgcmp, p);
            cmp=dbgcmp; mpf_clear(lhs); *pp=p; return cmp; }
        mpf_t rhs; mpf_init2(rhs, g_prec);
        p = sk(eval_expr(sk(p + oplen), rhs));
        int c = mpf_cmp(lhs, rhs);
        if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
        else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
        else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
        else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
        else cmp=(c==0);
        mpf_clears(lhs, rhs, NULL);
    }
    *pp = p;
    return cmp;
}

static char *find_else(char *p) {
    int in_str = 0;
    while (*p) {
        if (*p == '"') { in_str = !in_str; p++; continue; }
        if (!in_str && kw_match(p, "ELSE")) return p;
        p++;
    }
    return NULL;
}

/* Skip forward past the block starting at start_pc (already inside the IF body)
 * to the next ELSEIF/ELSE/END IF at the same nesting depth.
 * Returns the pc of that line, or g_nlines if not found. */
static int find_block_branch(int start_pc) {
    int depth = 0;
    for (int pc = start_pc; pc < g_nlines; pc++) {
        char *t = sk(g_lines[pc].text);
        /* nested block IF: no inline body after THEN */
        if (kw_match(t, "IF")) {
            char *rest = t + 2;
            /* scan for THEN at end of line */
            char *th = NULL;
            int in_s = 0;
            for (char *q = rest; *q; q++) {
                if (*q == '"') { in_s = !in_s; continue; }
                if (!in_s && kw_match(q, "THEN")) th = q;
            }
            if (th) {
                char *after = sk(th + 4);
                if (!*after || *after == '\'') depth++;  /* block IF */
            }
            continue;
        }
        if (kw_match(t, "END") && kw_match(sk(t+3), "IF")) {
            if (depth == 0) return pc;
            depth--;
            continue;
        }
        if (depth == 0 && (kw_match(t, "ELSEIF") || kw_match(t, "ELSE")))
            return pc;
    }
    return g_nlines;
}

static int cmd_if(Interp *ip, char *args) {
    char *p = sk(args);
    int result = eval_bool_expr(&p);
    p = sk(p);
    if (kw_match(p, "THEN")) p = sk(p + 4);

    /*  Single-line IF: something follows THEN on the same line  */
    if (*p && *p != '\'' && *p != '\0') {
        char *else_p = find_else(p);
        if (result) {
            char then_clause[MAX_LINE_LEN];
            if (else_p) {
                size_t len = (size_t)(else_p - p);
                if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
                memcpy(then_clause, p, len); then_clause[len] = '\0';
                p = then_clause;
            }
            if (isdigit((unsigned char)*p)) return cmd_goto(ip, p);
            int jumped = dispatch_multi(ip, p);
            if (!jumped) ip->pc++;
            return 1;
        } else {
            ip->pc++;
            if (!else_p) return 1;
            p = sk(else_p + 4);
            if (isdigit((unsigned char)*p)) return cmd_goto(ip, p);
            dispatch_multi(ip, p);
            return 1;
        }
    }

    /*  Block IF: nothing (or comment) after THEN  */
    if (result) {
        /* Execute the body  run loop will hit ELSEIF/ELSE/END IF naturally.
         * We just advance into the body; the ELSEIF/ELSE/END IF handlers
         * will skip the remaining branches. */
        ip->pc++;
        return 1;
    } else {
        /* Skip to first ELSEIF/ELSE/END IF at this depth */
        int branch = find_block_branch(ip->pc + 1);
        if (branch >= g_nlines) { ip->pc = g_nlines; return 1; }
        char *t = sk(g_lines[branch].text);
        if (kw_match(t, "ELSEIF")) {
            /* treat as a new IF on that line */
            ip->pc = branch;
            char *ei_args = sk(t + 6);
            return cmd_if(ip, ei_args);
        } else if (kw_match(t, "ELSE")) {
            /* execute from the line after ELSE */
            ip->pc = branch + 1;
            return 1;
        } else {
            /* END IF  step past it */
            ip->pc = branch + 1;
            return 1;
        }
    }
}

/* ELSEIF / ELSE / END IF  only reached when we're executing a taken branch
 * and need to skip to END IF */
static int cmd_elseif(Interp *ip, char *args) {
    (void)args;
    /* We're inside a taken IF/ELSEIF branch and have hit the next ELSEIF 
     * skip forward to END IF */
    int end = find_block_branch(ip->pc + 1);
    /* find_block_branch stops at ELSEIF/ELSE/END IF; keep skipping until END IF */
    while (end < g_nlines) {
        char *t = sk(g_lines[end].text);
        if (kw_match(t, "END") && kw_match(sk(t+3), "IF")) break;
        end = find_block_branch(end + 1);
    }
    ip->pc = (end < g_nlines) ? end + 1 : g_nlines;
    return 1;
}

static int cmd_else(Interp *ip, char *args) {
    (void)args;
    /* Reached ELSE while executing a taken IF branch  skip to END IF */
    int depth = 0;
    int pc = ip->pc + 1;
    while (pc < g_nlines) {
        char *t = sk(g_lines[pc].text);
        if (kw_match(t, "IF")) {
            char *rest = t + 2; char *th = NULL; int in_s = 0;
            for (char *q = rest; *q; q++) {
                if (*q == '"') { in_s = !in_s; continue; }
                if (!in_s && kw_match(q, "THEN")) th = q;
            }
            if (th && !*sk(th + 4)) depth++;
        } else if (kw_match(t, "END") && kw_match(sk(t+3), "IF")) {
            if (depth == 0) { ip->pc = pc + 1; return 1; }
            depth--;
        }
        pc++;
    }
    ip->pc = g_nlines;
    return 1;
}

/* ================================================================
 * DATA / READ / RESTORE
 * ================================================================ */
static int cmd_data(Interp *ip, char *args)    { (void)ip;(void)args; return 0; }
static int cmd_restore(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!*p || *p == ':' || *p == '\'') {
        g_data_pos = 0;
        return 0;
    }
    /* RESTORE label or RESTORE linenum  seek to first DATA item at or after that line */
    int target_idx = -1;
    if (isdigit((unsigned char)*p)) {
        int linenum = atoi(p);
        target_idx = find_line_idx(linenum);
    } else {
        char lname[MAX_VARNAME]; int i = 0;
        while ((isalnum((unsigned char)*p) || *p == '_') && i < MAX_VARNAME - 1)
            lname[i++] = *p++;
        lname[i] = '\0';
        target_idx = find_line_by_label(lname);
    }
    if (target_idx < 0) { g_data_pos = 0; return 0; }
    /* Find the first data item whose line index >= target_idx */
    for (int i = 0; i < g_data_count; i++) {
        if (g_data_line[i] >= target_idx) { g_data_pos = i; return 0; }
    }
    g_data_pos = g_data_count;  /* past end  no data found after label */
    return 0;
}

static int cmd_read(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        int arr_i = 0, arr_j = 1, is_arr = 0;
        if (*p == '(') {
            is_arr = 1; p = sk(p + 1);
            mpf_t i1; mpf_init2(i1, g_prec);
            p = sk(eval_expr(p, i1)); arr_i = (int)mpf_get_si(i1); mpf_clear(i1);
            if (*p==',') { p=sk(p+1); mpf_t i2; mpf_init2(i2,g_prec); p=sk(eval_expr(p,i2)); arr_j=(int)mpf_get_si(i2); mpf_clear(i2); }
            if (*p==')') p = sk(p + 1);
        }
        if (g_data_pos >= g_data_count) { basic_stderr("READ: out of data\n"); return -1; }
        char *item = g_data[g_data_pos++];
        Var *v = var_get(name);
        if (var_is_str_name(name)) {
            if (is_arr && v->kind == VAR_ARRAY_STR) {
                char **slot = arr_str_elem(v, arr_i, arr_j);
                free(*slot); *slot = str_dup(item);
            } else { free(v->str); v->str = str_dup(item); }
        } else {
            if (is_arr) { mpf_t *e = arr_num_elem(v, arr_i, arr_j); mpf_set_d(*e, atof(item)); var_fix_int(v, *e); }
            else        { mpf_set_d(v->num, atof(item)); var_fix_int(v, v->num); }
        }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

/* ================================================================
 * DEF FN
 * ================================================================ */
static int cmd_def(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!(toupper((unsigned char)p[0])=='F' && toupper((unsigned char)p[1])=='N'
          && isalnum((unsigned char)p[2]))) return 0;
    p += 2;
    if (g_defn_count >= MAX_DEF_FN) return 0;
    DefFn *fn = &g_defn[g_defn_count++];

    char namebuf[MAX_VARNAME];
    int i = 0;
    namebuf[i++] = 'F'; namebuf[i++] = 'N';
    while (isalnum((unsigned char)*p) && i < MAX_VARNAME - 1) namebuf[i++] = (char)toupper(*p++);
    namebuf[i] = '\0';
    fn->name = bstrdup(namebuf);

    p = sk(p);
    char parambuf[MAX_VARNAME];
    parambuf[0] = '\0';
    if (*p == '(') {
        p = sk(p + 1); int j = 0;
        while (*p && *p != ')' && j < MAX_VARNAME - 1) parambuf[j++] = (char)toupper(*p++);
        parambuf[j] = '\0';
        if (*p == ')') p++;
    }
    fn->param = bstrdup(parambuf);

    p = sk(p); if (*p == '=') p = sk(p + 1);
    fn->body = bstrdup(p);
    return 0;
}

/* ================================================================
 * ON x GOTO / ON x GOSUB
 * ================================================================ */
static int cmd_on(Interp *ip, char *args) {
    char *p = sk(args);
    mpf_t val; mpf_init2(val, g_prec);
    p = sk(eval_expr(p, val));
    int idx = (int)mpf_get_d(val);
    mpf_clear(val);
    int is_gosub = 0;
    if      (kw_match(p,"GOSUB")) { is_gosub=1; p=sk(p+5); }
    else if (kw_match(p,"GOTO"))  {             p=sk(p+4); }
    else return 0;
    int targets[64]; int nt = 0;
    while (*p && nt < 64) {
        p = sk(p);
        if (!isdigit((unsigned char)*p)) break;
        targets[nt++] = atoi(p);
        while (isdigit((unsigned char)*p)) p++;
        p = sk(p); if (*p == ',') p++;
    }
    if (idx < 1 || idx > nt) return 0;
    char num[32]; snprintf(num, sizeof num, "%d", targets[idx - 1]);
    return is_gosub ? cmd_gosub(ip, num) : cmd_goto(ip, num);
}

/* ================================================================
 * DEFINT / DEFSNG / DEFDBL / DEFSTR stubs
 * ================================================================ */
/* DEFINT A-Z, B: mark (or unmark) the letters' default type as integer. */
static int def_letters(char *args, int is_int) {
    char *p = sk(args);
    while (isalpha((unsigned char)*p)) {
        int a = toupper((unsigned char)*p) - 'A', b = a;
        p = sk(p + 1);
        if (*p == '-') {
            p = sk(p + 1);
            if (isalpha((unsigned char)*p)) { b = toupper((unsigned char)*p) - 'A'; p = sk(p + 1); }
        }
        if (a > b) { int t = a; a = b; b = t; }
        for (int i = a; i <= b; i++) g_defint[i] = (unsigned char)is_int;
        if (*p != ',') break;
        p = sk(p + 1);
    }
    return 0;
}
static int cmd_defint(Interp *ip, char *args) { (void)ip; return def_letters(args, 1); }
static int cmd_defsng(Interp *ip, char *args) { (void)ip; return def_letters(args, 0); }
static int cmd_defstr(Interp *ip, char *args) { (void)ip; (void)args; return 0; }  /* names keep their $ */

/* ================================================================
 * Command registration table
 * ================================================================ */
const Command commands[] = {
    /* Hot loop commands first (Mandelbrot inner loop hits these 1000x/sec) */
    { "WHILE",      cmd_while      },
    { "WEND",       cmd_wend       },
    { "FOR",        cmd_for        },
    { "NEXT",       cmd_next       },
    
    /* Common assignment/output */
    { "LET",        cmd_let        },
    { "PRINT",      cmd_print      },
    
    /* Graphics (called in draw loops) */
    { "PSET",       cmd_pset       },
    
    /* Everything else */
    { "DECLARE",    cmd_rem        },
    { "REM",        cmd_rem        },
    { "'",          cmd_rem        },
    { "SYSTEM",     cmd_system     },
    { "SLEEP",      cmd_sleep      },
    { "KILL",       cmd_kill       },
    { "GET",        cmd_get_graphics },
    { "PUT",        cmd_put_graphics },
    { "DRAW",       cmd_draw       },
    { "CIRCLE",     cmd_circle     },
    { "PRESET",     cmd_preset     },
    { "PAINT",      cmd_paint      },
    { "END SELECT", cmd_end_select },
    { "END SUB",    cmd_end_sub    },
    { "END FUNCTION",cmd_end_sub   },
    { "END IF",     cmd_rem        },
    { "END",        cmd_end        },
    { "SUB",        cmd_rem        },
    { "FUNCTION",   cmd_rem        },
    { "EXIT",       cmd_exit       },
    { "STOP",       cmd_stop       },
    { "CONT",       cmd_cont       },
    { "RANDOMIZE",  cmd_randomize  },
    { "SWAP",       cmd_swap       },
    { "ERASE",      cmd_erase      },
    { "OPTION",     cmd_option     },
    { "CONST",      cmd_const      },
    { "DEBUG",      cmd_debug      },
    { "CLS",        cmd_cls        },
    { "BEEP",       cmd_beep       },
    { "WINDOW",     cmd_window     },
    { "_DISPLAY",   cmd_qdisplay   },
    { "_TITLE",     cmd_qtitle     },
    { "_LIMIT",     cmd_qlimit     },
    { "SOUND",      cmd_sound      },
    { "PLAY",       cmd_play       },
    { "COLOR",      cmd_color      },
    { "LOCATE",     cmd_locate     },
    { "WIDTH",      cmd_width      },
    { "SCREEN",     cmd_screen     },
    { "KEY",        cmd_key        },
    { "PALETTE",    cmd_palette    },
    { "POKE",       cmd_poke       },
    { "VIEW PRINT", cmd_view_print },
    { "VIEW",       cmd_rem        },
    { "REDIM",      cmd_redim      },
    { "DIM",        cmd_dim        },
    { "STATIC",     cmd_static     },
    { "DO",         cmd_do         },
    { "LOOP",       cmd_loop       },
    { "SELECT",     cmd_select     },
    { "CASE",       cmd_case       },  /* reached after a case body completes  jump to END SELECT */
    { "GOTO",       cmd_goto       },
    { "GOSUB",      cmd_gosub      },
    { "RETURN",     cmd_return     },
    { "CALL",       cmd_call       },
    { "IF",         cmd_if         },
    { "ELSEIF",     cmd_elseif     },
    { "ELSE",       cmd_else       },
    { "LINE INPUT", cmd_line_input },
    { "LINE",       cmd_line_gfx   },
    { "INPUT",      cmd_input      },
    { "OPEN",       cmd_open       },
    { "CLOSE",      cmd_close      },
    { "DEF SEG",    cmd_defseg     },
    { "DEF",        cmd_def        },
    { "DEFDBL",     cmd_defdbl     },
    { "DEFINT",     cmd_defint     },
    { "DEFSNG",     cmd_defsng     },
    { "DEFDBL",     cmd_defsng     },
    { "DEFLNG",     cmd_defint     },
    { "DEFSTR",     cmd_defstr     },
    { "ON ERROR",   cmd_on_error   },
    { "ON",         cmd_on         },
    { "RESUME",     cmd_resume     },
    { "ERROR",      cmd_error      },
    { "WRITE",      cmd_write      },
    { "TRON",       cmd_tron       },
    { "TROFF",      cmd_troff      },
    { "LPRINT",     cmd_lprint     },
    { "LLIST",      cmd_llist      },
    { "OUT",        cmd_out        },
    { "WAIT",       cmd_wait       },
    { "MOTOR",      cmd_motor      },
    { "READ",       cmd_read       },
    { "DATA",       cmd_data       },
    { "RESTORE",    cmd_restore    },
    { "RUN",        cmd_run        },
    { "CHAIN",      cmd_chain      },
    { "FULLSCREEN", cmd_fullscreen },
    { "DELAY",      cmd_delay      },
    { "SLEEP",      cmd_delay      },
    { NULL,         NULL           }
};

/* ================================================================
 * Statement splitter  splits a line on unquoted colons
 * ================================================================ */
static int split_statements(char *line, char *segs[], char **buf_out) {
    char *buf = str_dup(line);
    *buf_out = buf;
    int n = 0, in_str = 0;
    segs[n++] = buf;
    char *trimmed = buf;
    while (isspace((unsigned char)*trimmed)) trimmed++;
    if (strncasecmp(trimmed,"REM",3)==0 && !isalnum((unsigned char)trimmed[3]) && trimmed[3]!='_') return n;
    if (*trimmed == '\'') return n;
    for (char *p = buf; *p; p++) {
        if (*p == '"') in_str = !in_str;
        if (!in_str && *p == ':') {
            char *rest = p + 1;
            while (isspace((unsigned char)*rest)) rest++;
            if (strncasecmp(rest,"REM",3)==0 && !isalnum((unsigned char)rest[3]) && rest[3]!='_')
                { *p = '\0'; break; }
            if (strncasecmp(rest,"IF",2)==0 && !isalnum((unsigned char)rest[2]) && rest[2]!='_')
                { *p = '\0'; if (n < MAX_STMTS) segs[n++] = rest; break; }
            *p = '\0';
            if (n < MAX_STMTS) segs[n++] = p + 1;
        }
    }
    return n;
}

/* ================================================================
 * Dispatcher
 * ================================================================ */
/* Format an mpf value sensibly:
 * - plain decimal for numbers that fit reasonably (exponent -6..15)
 * - scientific notation with trailing zeros trimmed otherwise */
static void print_mpf(mpf_t val) {
    int bufsz = PRINT_DIGITS + 64;
    char *buf = (char *)malloc(bufsz);
    if (!buf) return;

    /* Get scientific form to inspect the exponent */
    char *tmp = (char *)malloc(bufsz);
    if (!tmp) { free(buf); return; }
#ifndef DONTUSEGMP
    gmp_snprintf(tmp, bufsz, "%.*Fe", PRINT_DIGITS, val);
#else
    snprintf(tmp, bufsz, "%.*e", PRINT_DIGITS, mpf_get_d(val));
#endif

    /* Parse exponent */
    char *ep = strchr(tmp, 'e');
    int exp = ep ? atoi(ep + 1) : 0;

    if (exp >= -6 && exp <= 15) {
        /* Plain decimal  figure out decimal places needed */
        int dp = PRINT_DIGITS - exp;
        if (dp < 0) dp = 0;
        if (dp > PRINT_DIGITS) dp = PRINT_DIGITS;
#ifndef DONTUSEGMP
        gmp_snprintf(buf, bufsz, "%.*Ff", dp, val);
#else
        snprintf(buf, bufsz, "%.*f", dp, mpf_get_d(val));
#endif
        /* Trim trailing zeros after decimal point */
        if (strchr(buf, '.')) {
            char *end = buf + strlen(buf) - 1;
            while (*end == '0') *end-- = '\0';
            if (*end == '.') *end = '\0';
        }
    } else {
        /* Scientific notation  trim trailing zeros in significand */
#ifndef DONTUSEGMP
        gmp_snprintf(buf, bufsz, "%.*Fe", PRINT_DIGITS, val);
#else
        snprintf(buf, bufsz, "%.*e", PRINT_DIGITS, mpf_get_d(val));
#endif
        char *e = strchr(buf, 'e');
        if (e) {
            char *z = e - 1;
            while (z > buf && *z == '0') z--;
            if (*z == '.') z--;
            memmove(z + 1, e, strlen(e) + 1);
        }
    }
    free(tmp);

    int len = (int)strlen(buf);
    if (len + 1 < bufsz) { buf[len] = '\n'; buf[len+1] = '\0'; }
    display_print(buf);
    free(buf);
}

int dispatch_one(Interp *ip, char *stmt, char *full_line) {
    char *p = sk(stmt);
    if (!*p) return 0;

    for (int i = 0; commands[i].keyword; i++) {
        char *kw = commands[i].keyword;
        size_t len = strlen(kw);
        if (strncasecmp(p, kw, len) == 0) {
            char next = p[len];
            if (!isalnum((unsigned char)next) && next != '_' && next != '$') {
                if (strcasecmp(kw,"IF") == 0 && full_line) {
                    char *fl = sk(full_line);
                    if (strncasecmp(fl,"IF",2) == 0) fl = sk(fl + 2);
                    return commands[i].fn(ip, fl);
                }
                return commands[i].fn(ip, sk(p + len));
            }
        }
    }

    /* Bare assignment: var = expr, arr(i) = expr, or var.field = expr */
    if (isalpha((unsigned char)*p)) {
        char name[MAX_VARNAME];
        char *after = read_varname(p, name);
        /* skip type sigil (#, !, %, &) after varname */
        if (*after == '#' || *after == '!' || *after == '%' || *after == '&') after++;
        after = sk(after);
        if (*after == '=') return cmd_let(ip, p);
        /* struct field: name.field = ...  ("Rest .02" is a SUB call with a number) */
        if (*after == '.' && !isdigit((unsigned char)after[1])) return cmd_let(ip, p);
        /* name(...)  peek past the argument list to decide: assignment or bare expression */
        if (*after == '(') {
            char *peek = after + 1;
            int depth = 1;
            while (*peek && depth > 0) {
                if (*peek == '(') depth++;
                else if (*peek == ')') depth--;
                peek++;
            }
            peek = sk(peek);
            /* struct field after subscript: arr(i).field = ... */
            if (*peek == '.') return cmd_let(ip, p);
            /* array assignment: arr(i) = ... */
            if (*peek == '=') return cmd_let(ip, p);
            /* Name (X): a SUB called with a parenthesized argument */
            {
                int si = find_line_by_label(name);
                if (si >= 0 && strncasecmp(sk(g_lines[si].text), "SUB", 3) == 0 &&
                    !isalnum((unsigned char)sk(g_lines[si].text)[3]))
                    return call_sub(ip, p, 1);
            }
            /* No '='  treat as a bare numeric expression and print the result.
             * Handles: tan(5), sin(3.14), sqr(2), (3+4)*2, etc. */
            {
                mpf_t result; mpf_init2(result, g_prec);
                extern jmp_buf g_parse_error_jmp;
                extern int g_parse_error_active;
                g_parse_error_active = 1;
                if (setjmp(g_parse_error_jmp) == 0) {
                    eval_expr(p, result);
                    print_mpf(result);
                }
                g_parse_error_active = 0;
                mpf_clear(result);
                return 0;
            }
        }
        /* Bare sub call without CALL keyword */
        return call_sub(ip, p, 1);
    }

    /* Bare numeric expression starting with unary sign, digit, or paren */
    if (*p == '-' || *p == '+' || *p == '(' || isdigit((unsigned char)*p)) {
        mpf_t result; mpf_init2(result, g_prec);
        extern jmp_buf g_parse_error_jmp;
        extern int g_parse_error_active;
        g_parse_error_active = 1;
        if (setjmp(g_parse_error_jmp) == 0) {
            eval_expr(p, result);
            print_mpf(result);
        }
        g_parse_error_active = 0;
        mpf_clear(result);
        return 0;
    }

    basic_stderr("Warning: unknown: %.60s\n", p);
    return 0;
}

int dispatch(Interp *ip, char *line) {
    return dispatch_one(ip, line, line);
}

int dispatch_multi(Interp *ip, char *clause) {
    char *segs[MAX_STMTS];
    char *buf;
    int n = split_statements(clause, segs, &buf);
    int jumped = 0;
    for (int i = 0; i < n && !jumped; i++)
        jumped = dispatch_one(ip, segs[i], segs[i]);
    free(buf);
    return jumped;
}

BASIC_NS_END
