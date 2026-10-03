#pragma once
/*
 * basic.h — Shared types, constants, and extern declarations for the
 *            BASIC interpreter.  Every .c file in this project includes
 *            this header and nothing else (beyond standard library headers).
 *
 * Build:
 *   gcc -O2 -o basic main.c vars.c expr.c program.c commands.c display_ansi.c \
 *           -lgmp -lm
 */

#include "basic_ns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#if defined(_MSC_VER) && !defined(__MINGW32__)
#include "msvc_posix_compat.h"  // dirent.h/unistd.h don't exist under MSVC
#else
#include <dirent.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <signal.h>
#ifndef DONTUSEGMP
#include <gmp.h>
#else

struct _mpf_struct { double val; };
typedef struct _mpf_struct mpf_t[1];
#define mp_bitcnt_t unsigned long

#define mpf_abs(dst, src) ((dst)[0].val = fabs((src)[0].val))
#define mpf_add(dst, x, y) ((dst)[0].val = (x)[0].val + (y)[0].val)
#define mpf_clear(x)  ((void)0)
#define mpf_clears(...) ((void)0)
#define mpf_cmp(a, b) (((a)[0].val > (b)[0].val) - ((a)[0].val < (b)[0].val))
#define mpf_div(dst, a, b) ((dst)[0].val = (a)[0].val / (b)[0].val)
#define mpf_get_d(x) ((x)[0].val)
#define mpf_get_si(x) ((long)(x)[0].val)
#define mpf_get_ui(x) ((unsigned long)(x)[0].val)
#define mpf_init2(x, prec) ((x)[0].val = 0.0)
#define mpf_mul(dst, a, b) ((dst)[0].val = (a)[0].val * (b)[0].val)
#define mpf_neg(dst, src) ((dst)[0].val = -(src)[0].val)
#define mpf_set(dst, src) ((dst)[0].val = (src)[0].val)
#define mpf_set_d(dst, n) ((dst)[0].val = (n))
#define mpf_set_default_prec(prec) ((void)0)
#define mpf_set_si(dst, n) ((dst)[0].val = (n))
#define mpf_set_str(dst, buf, base) ((dst)[0].val = strtod((buf), NULL))
#define mpf_set_ui(dst, n) ((dst)[0].val = (n))
#define mpf_sgn(x) (((x)[0].val > 0) - ((x)[0].val < 0))
#define mpf_sub(dst, x, y) ((dst)[0].val = (x)[0].val - (y)[0].val)

#endif


#include "display.h"
#include "sound.h"



/* Hosted builds: g_break expands to this global (defined in main.cpp). */
#if defined(WOPR) || defined(FELIX_BASIC)
extern volatile sig_atomic_t BASIC_BREAK_SYM;
#endif

BASIC_NS_BEGIN

/* ================================================================
 * Configuration constants
 * ================================================================ */
#ifndef CTRL_STACK_MAX
#define CTRL_STACK_MAX    16384
#endif
#define DEFAULT_PREC     128
#define DEFAULT_BUFFER  4096
#define MAX_ARRAY_DIMS     2
/* The largest single array DIM accepts (elements). Arrays are allocated
 * at DIM, sized to what was asked for. Builds with little RAM (the
 * bare-metal kernel) pass smaller values for MAX_VARS, CTRL_STACK_MAX and
 * MAX_VARNAME (the control stack and the label/type tables) with -D. */
#ifndef MAX_ARRAY_SIZE
#define MAX_ARRAY_SIZE  65536
#endif
#define MAX_DATA_ITEMS  4096
#define MAX_DEF_FN        32
#define MAX_FILE_HANDLES  16
#define MAX_LINES       8192
#define MAX_LINE_LEN    16384   /* upper bound for a single line/body — the fields that use
                                    this (Line::text, DefFn::body, ConstEntry::value) are now
                                    allocated dynamically to the actual string length; this is
                                    just a sanity cap used when reading/validating input */
#define MAX_STMTS       16384
#ifndef MAX_VARNAME
#define MAX_VARNAME     1024    /* upper bound for identifiers — same story, see above */
#endif
#ifndef MAX_VARS
#define MAX_VARS        16384
#endif
#define PRINT_DIGITS      60

/* Small strdup helper — avoids relying on POSIX strdup() (not guaranteed
 * under -std=c++17 strict mode) for the dynamic-allocation fields below. */
static inline char *bstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ================================================================
 * Global interpreter settings
 * ================================================================ */
#ifndef DONTUSEGMP
extern mp_bitcnt_t      g_prec;
#else
extern int              g_prec;
#endif
extern int              g_option_base;
/* g_break: in hosted builds it is a macro (defined in basic_ns.h) that
 * expands to ::BASIC_BREAK_SYM.  In standalone builds it is a normal extern. */
#if !defined(WOPR) && !defined(FELIX_BASIC)
extern volatile sig_atomic_t g_break;
#endif
extern int              g_cont_pc;
extern int              g_current_pc;  /* pc of the line currently executing */
extern char             g_error_handler[MAX_VARNAME];
extern int              g_error_resume_pc;
extern int              g_err;   /* last error code (ERR) */
extern int              g_erl;   /* line number of last error (ERL) */
extern int              g_tron;  /* TRON trace flag */

/* ================================================================
 * File handle table
 * ================================================================ */
typedef struct {
    FILE *fp;
    char  mode;   /* 'I'=input, 'O'=output, 'A'=append, 0=closed */
} FileHandle;

extern FileHandle g_files[MAX_FILE_HANDLES + 1];  /* 1-based */

FileHandle *fh_get(int n);

/* ================================================================
 * Variable store
 * ================================================================ */
typedef enum { VAR_NUM, VAR_STR, VAR_ARRAY_NUM, VAR_ARRAY_STR } VarKind;

typedef struct {
    char   *name;        /* dynamically allocated (bstrdup), not fixed-size */
    VarKind kind;
    /* scalar */
    mpf_t   num;
    char   *str;
    /* array (up to 2D), allocated by DIM: arr_len elements of one kind */
    int     dim[MAX_ARRAY_DIMS];
    int     ndim;
    int     arr_len;
    mpf_t  *arr_num;
    char  **arr_str;                  /* the strings themselves are malloc'd too */
    int     scope;                    /* 0: the main program; n: the n-th procedure call deep */
} Var;

extern Var* g_vars;
extern int  g_nvar;

int     var_is_str_name(char *name);
Var    *var_find(char *name);
Var    *var_create(char *name);
Var    *var_get(char *name);
/* Procedure scopes. A program that declares SHARED variables gets QBasic's
 * rules: a SUB or FUNCTION has its own variables, which start at 0 or ""
 * on each call, and sees the main program's only where they're SHARED.
 * A program with no SHARED keeps every variable global. */
extern int g_scope;
extern int g_locals;
void    scope_program_start(void);    /* RUN: find the SHARED names, back to the main program */
int     scope_enter(void);            /* a procedure call: its scope, 0 when variables are global */
void    scope_leave(int scope);       /* its variables go */
Var    *var_find_in(char *name, int scope);
int     var_scope_for(char *name, int scope);   /* the scope a name means, seen from scope */
Var    *var_get_in(char *name, int scope);
void    sprite_forget(Var *v);        /* commands.cpp: a variable going away */
mpf_t  *arr_num_elem(Var *v, int i, int j);
void    basic_frame_tick(void);       /* present the screen now and then */
int     basic_paced(void);            /* the program is pacing itself with pauses */
void    var_free_arrays(Var *v);      /* release an array's elements and storage */
bool    var_alloc_array(Var *v, int total, int is_str);
char  **arr_str_elem(Var *v, int i, int j);
Var    *field_array(const char *base, const char *field, int *is_str);

/* ================================================================
 * Program store
 * ================================================================ */
typedef struct {
    int  linenum;
    char *text;   /* dynamically allocated (bstrdup), not fixed-size */
} Line;

extern Line* g_lines;
extern int  g_nlines;

int  line_cmp(const void *a, const void *b);
int  find_line_idx(int num);
int  find_line_by_label(char *name);
void normalize_kw(char *src, char *dst, int dstsz);
void load(char *filename);
void save_program(char *filename);
void load_program(char *filename);
void clear_program(void);

/* ================================================================
 * DATA / READ / RESTORE
 * ================================================================ */
extern char *g_data[MAX_DATA_ITEMS];


//extern int   g_data_line[MAX_DATA_ITEMS];  /* program line index of each DATA item */
extern int* g_data_line;

extern int   g_data_count;
extern int   g_data_pos;

void prescan_data(void);

/* ================================================================
 * Control stack — FOR loops and GOSUB frames
 * ================================================================ */
typedef struct {
    char  varname[MAX_VARNAME];  /* "\x01GOSUB" for subroutine frames */
    mpf_t limit, step;
    int   line_idx;              /* FOR: loop start; GOSUB: return address */
} CtrlFrame;

extern CtrlFrame* g_ctrl;
extern int       g_ctrl_top;

/* ================================================================
 * TYPE / struct definitions
 *
 * TYPE PlayerData
 *   PNam AS STRING * 17
 *   XCoor AS DOUBLE
 * END TYPE
 *
 * We store each TYPE definition as a list of field names (in order).
 * At runtime, TYPE variables are "flat": PDat(1).PNam is stored as
 * the variable "PDAT.1.PNAM" (for array element) or "PDAT.PNAM"
 * (for scalar).  This avoids a full struct runtime.
 * ================================================================ */
#define MAX_TYPE_DEFS   32
#define MAX_TYPE_FIELDS 64

typedef struct {
    char name[MAX_VARNAME];
    int  is_str;   /* 1 = string field, 0 = numeric */
} TypeField;

typedef struct {
    char      name[MAX_VARNAME];
    TypeField fields[MAX_TYPE_FIELDS];
    int       nfields;
} TypeDef;

extern TypeDef* g_typedefs;
extern int     g_ntypedefs;

TypeDef *typedef_find(char *name);

/* ================================================================
 * DEF FN store
 * ================================================================ */
typedef struct {
    char *name;    /* dynamically allocated (bstrdup), not fixed-size */
    char *param;
    char *body;
} DefFn;

extern DefFn g_defn[MAX_DEF_FN];
extern int   g_defn_count;

/* ================================================================
 * Interpreter state
 * ================================================================ */
typedef struct {
    int pc;
    int running;
} Interp;

/* ================================================================
 * Utility helpers (defined in expr.c, used everywhere)
 * ================================================================ */
char        *str_dup(char *s);
char  *sk(char *p);
char  *read_varname(char *p, char *name);
int          kw_match(char *p, char *kw);
int          is_str_token(char *p);

/* ================================================================
 * CONST table (expr.c)
 * ================================================================ */
void const_clear(void);
void const_set(char *name, char *value, int is_str);
char *eval_expr(char *s, mpf_t result);
char *eval_str_expr(char *s, char *buf, int bufsz);
char *eval_str_or_inkey(char *p, char *buf, int bufsz);

/* ================================================================
 * Command dispatch (commands.c)
 * ================================================================ */
typedef int (*CmdFn)(Interp *ip, char *args);
typedef struct { char *keyword; CmdFn fn; } Command;

extern const Command commands[];

int dispatch_one(Interp *ip, char *stmt, char *full_line);
int dispatch(Interp *ip, char *line);
int dispatch_multi(Interp *ip, char *clause);

/* Individual command handlers needed by other modules */
int cmd_goto(Interp *ip, char *args);
int cmd_gosub(Interp *ip, char *args);

/* Sprite registry reset — call on program clear/restart */
void sprites_reset(void);

/* ================================================================
 * Main interpreter loop (main.c)
 * ================================================================ */
void run(void);
void run_from(int start_pc);

#if defined(WOPR) || defined(FELIX_BASIC)
int basic_main(void);
#else
int basic_main(int argc, char **argv);
#endif

BASIC_NS_END
