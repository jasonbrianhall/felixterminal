/*
 * vars.c — Variable store: scalar and array, numeric (mpf) and string.
 */
#include "basic.h"
#include <setjmp.h>

#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

BASIC_NS_BEGIN


/* ================================================================
 * Global state
 * ================================================================ */
Var* g_vars = nullptr;

struct VarsInit {
    VarsInit() {
        g_vars = new Var[MAX_VARS]();
    }
    ~VarsInit() {
        for (int i = 0; i < g_nvar; i++) {
            if (!g_vars[i].name) continue;
            free(g_vars[i].name);
            if (g_vars[i].kind == VAR_STR) free(g_vars[i].str);
            var_free_arrays(&g_vars[i]);
        }
        delete[] g_vars;
    }
};

static VarsInit _vars_init;





int  g_nvar = 0;

TypeDef* g_typedefs = nullptr;

struct TypeDefsInit {
    TypeDefsInit() {
        g_typedefs = new TypeDef[MAX_TYPE_DEFS];
    }
    ~TypeDefsInit() {
        delete[] g_typedefs;
    }
};

static TypeDefsInit _typedefs_init;

int     g_ntypedefs = 0;

TypeDef *typedef_find(char *name) {
    for (int i = 0; i < g_ntypedefs; i++)
        if (strcasecmp(g_typedefs[i].name, name) == 0) return &g_typedefs[i];
    return NULL;
}

FileHandle g_files[MAX_FILE_HANDLES + 1];  /* 1-based */

/* ================================================================
 * File handle
 * ================================================================ */
FileHandle *fh_get(int n) {
    extern jmp_buf g_parse_error_jmp;
    extern int g_parse_error_active;
    
    if (n < 1 || n > MAX_FILE_HANDLES) {
        basic_stderr("Bad file number: %d\n", n);
        if (g_parse_error_active) {
            longjmp(g_parse_error_jmp, 1);
        } else {
            exit(1);
        }
    }
    return &g_files[n];
}

/* ================================================================
 * Variable helpers
 * ================================================================ */
int var_is_str_name(char *name) {
    return name[strlen(name) - 1] == '$';
}

/* INKEY$'s string for key code ch: "" for none, one character, or for an
 * extended key CHR$(0) + its PC scan code: the arrows (0x1000-0x1003 from
 * the display layer) and KEY_EXT(scan) for F1-F12, Home, End, PgUp, PgDn,
 * Ins and Del. */
int inkey_to_str(int ch, char *buf) {
    static const char arrows[4] = { 'H', 'P', 'K', 'M' };   /* up down left right */
    if (ch >= 0x1000 && ch <= 0x1003) {
        buf[0] = BASIC_NUL_CH; buf[1] = arrows[ch - 0x1000]; buf[2] = '\0';
        return 2;
    }
    if (ch > KEY_EXT(0) && ch <= KEY_EXT(255)) {
        buf[0] = BASIC_NUL_CH; buf[1] = (char)(ch & 0xFF); buf[2] = '\0';
        return 2;
    }
    if (ch <= 0 || ch > 255) { buf[0] = '\0'; return 0; }
    buf[0] = (char)ch; buf[1] = '\0';
    return 1;
}

/* ---------------------------------------------------------------- types */
/* Each letter's default type, as DEFINT / DEFLNG / DEFSNG / DEFDBL / DEFSTR
 * set it: the suffix a name without one stands for ('!' unless changed). */
char g_deftype[26];

static void deftype_reset(void) { memset(g_deftype, '!', sizeof g_deftype); }

/* DEFINT A-Z, B (type '%', '&', '!', '#' or '$'). */
void def_letters_apply(const char *p, char type) {
    if (!g_deftype[0]) deftype_reset();
    while (*p == ' ' || *p == '\t') p++;
    while (isalpha((unsigned char)*p)) {
        int a = toupper((unsigned char)*p) - 'A', b = a;
        p++;
        while (*p == ' ') p++;
        if (*p == '-') {
            p++;
            while (*p == ' ') p++;
            if (isalpha((unsigned char)*p)) { b = toupper((unsigned char)*p) - 'A'; p++; }
        }
        if (a > b) { int t = a; a = b; b = t; }
        for (int i = a; i <= b; i++) g_deftype[i] = type;
        while (*p == ' ') p++;
        if (*p != ',') break;
        p++;
        while (*p == ' ') p++;
    }
}

/* The name a variable is stored under. A numeric suffix that only repeats
 * the letter's default type is dropped, so after DEFINT A-Z, A and A% are
 * one variable, as in QBasic (and after none, A and A!). */
static const char *var_canon(const char *name, char *buf) {
    size_t n = strlen(name);
    if (n < 2 || n >= MAX_VARNAME) return name;
    char last = name[n - 1];
    if (last != '%' && last != '&' && last != '!' && last != '#') return name;
    int c = toupper((unsigned char)name[0]);
    if (c < 'A' || c > 'Z') return name;
    char def = g_deftype[0] ? g_deftype[c - 'A'] : '!';
    if (def != last) return name;
    memcpy(buf, name, n - 1);
    buf[n - 1] = '\0';
    return buf;
}

/* Is a plain variable of this name an integer: A% / A&, or no suffix and a
 * first letter DEFINT / DEFLNG covers. */
int var_name_is_int(const char *name) {
    size_t n = strlen(name);
    if (!n) return 0;
    char last = name[n - 1];
    if (last == '%' || last == '&') return 1;
    if (last == '$' || last == '!' || last == '#') return 0;
    int c = toupper((unsigned char)name[0]);
    if (c < 'A' || c > 'Z' || !g_deftype[0]) return 0;
    return g_deftype[c - 'A'] == '%' || g_deftype[c - 'A'] == '&';
}

/* Storing into an INTEGER or LONG rounds to the nearest whole number, an
 * exact half to the even one (CINT's rule). */
void var_fix_int(Var *v, mpf_t x) {
    if (!v || !v->is_int) return;
    double d = mpf_get_d(x), f = floor(d), r = d - f;
    if (r > 0.5 || (r == 0.5 && fmod(f, 2.0) != 0)) f += 1;
    mpf_set_d(x, f);
}

/* ---------------------------------------------------------------- scopes */
int g_scope  = 0;
int g_locals = 0;

static char **g_shared;      /* names declared SHARED anywhere in the program */
static int    g_nshared, g_capshared;

static void shared_add(const char *name, int len) {
    if (len <= 0 || len >= MAX_VARNAME) return;
    char raw[MAX_VARNAME], cb[MAX_VARNAME];
    memcpy(raw, name, (size_t)len); raw[len] = '\0';
    name = var_canon(raw, cb); len = (int)strlen(name);
    for (int i = 0; i < g_nshared; i++)
        if ((int)strlen(g_shared[i]) == len && strncasecmp(g_shared[i], name, len) == 0) return;
    if (g_nshared == g_capshared) {
        int cap = g_capshared ? g_capshared * 2 : 32;
        char **n = (char **)realloc(g_shared, (size_t)cap * sizeof(char *));
        if (!n) return;
        g_shared = n; g_capshared = cap;
    }
    char *c = (char *)malloc((size_t)len + 1);
    if (!c) return;
    memcpy(c, name, (size_t)len); c[len] = 0;
    g_shared[g_nshared++] = c;
}

/* A name seen from inside a procedure: one of the main program's SHARED
 * variables? (For a TYPE variable, P.X goes by P.) */
static int var_is_shared(const char *name) {
    char cb[MAX_VARNAME];
    name = var_canon(name, cb);
    int len = 0;
    while (name[len] && name[len] != '.') len++;
    for (int i = 0; i < g_nshared; i++)
        if ((int)strlen(g_shared[i]) == len && strncasecmp(g_shared[i], name, len) == 0) return 1;
    return 0;
}

static int kw_at(const char *p, const char *kw) {
    size_t n = strlen(kw);
    return strncasecmp(p, kw, n) == 0 && !isalnum((unsigned char)p[n]) && p[n] != '_';
}

/* The names in "DIM SHARED A, B(10), C AS INTEGER" and the like. */
static void shared_scan_list(const char *p) {
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        const char *n = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        if (*p == '$' || *p == '!' || *p == '#' || *p == '%' || *p == '&') p++;
        if (p == n) return;
        shared_add(n, (int)(p - n));
        while (*p == ' ') p++;
        if (*p == '(') {                         /* array bounds */
            int depth = 0;
            do { if (*p == '(') depth++; else if (*p == ')') depth--; p++; } while (*p && depth > 0);
            while (*p == ' ') p++;
        }
        if (kw_at(p, "AS")) {                   /* AS type */
            p += 2;
            while (*p == ' ') p++;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            while (*p == ' ') p++;
        }
        if (*p != ',') return;
        p++;
    }
}

void scope_program_start(void) {
    /* DEFINT and the rest are declarations: apply them all up front, so
     * names are stored the same way however the program runs. */
    deftype_reset();
    for (int i = 0; i < g_nlines; i++) {
        const char *t = g_lines[i].text;
        if (!t) continue;
        while (*t == ' ' || *t == '\t') t++;
        static const struct { const char *kw; char type; } defs[] = {
            { "DEFINT", '%' }, { "DEFLNG", '&' }, { "DEFSNG", '!' }, { "DEFDBL", '#' }, { "DEFSTR", '$' } };
        for (auto &d : defs)
            if (kw_at(t, d.kw)) { def_letters_apply(t + 6, d.type); break; }
    }
    for (int i = 0; i < g_nshared; i++) free(g_shared[i]);
    g_nshared = 0;
    g_scope = 0;
    int any = 0;
    for (int i = 0; i < g_nlines; i++) {
        const char *t = g_lines[i].text;
        if (!t) continue;
        while (*t == ' ' || *t == '\t') t++;
        if (kw_at(t, "DIM") || kw_at(t, "REDIM") || kw_at(t, "COMMON")) {
            t += kw_at(t, "DIM") ? 3 : kw_at(t, "REDIM") ? 5 : 6;
            while (*t == ' ') t++;
            if (kw_at(t, "PRESERVE")) { t += 8; while (*t == ' ') t++; }
            if (!kw_at(t, "SHARED")) continue;
            t += 6;
        } else if (kw_at(t, "SHARED")) {
            t += 6;
        } else continue;
        any = 1;
        shared_scan_list(t);
    }
    g_locals = any;
}

int scope_enter(void) {
    if (!g_locals) return 0;
    return ++g_scope;
}

static void var_release(Var *v) {
    sprite_forget(v);
    free(v->name);
    if (v->kind == VAR_STR) free(v->str);
    else if (v->kind == VAR_NUM) mpf_clear(v->num);
    var_free_arrays(v);
    memset(v, 0, sizeof(*v));                    /* name NULL: a free slot */
}

void scope_leave(int scope) {
    if (scope <= 0) return;
    for (int i = 0; i < g_nvar; i++)
        if (g_vars[i].name && g_vars[i].scope >= scope) var_release(&g_vars[i]);
    while (g_nvar > 0 && !g_vars[g_nvar - 1].name) g_nvar--;
    if (g_scope >= scope) g_scope = scope - 1;
}

/* Which scope a name means, seen from scope sc. */
static int scope_of(char *name, int sc) {
    return (sc > 0 && g_locals && !var_is_shared(name)) ? sc : 0;
}

int var_scope_for(char *name, int sc) { return scope_of(name, sc); }

Var *var_find_in(char *name, int sc) {
    char cb[MAX_VARNAME];
    name = (char *)var_canon(name, cb);
    int want = scope_of(name, sc);
    for (int i = 0; i < g_nvar; i++)
        if (g_vars[i].name && g_vars[i].scope == want && strcasecmp(g_vars[i].name, name) == 0)
            return &g_vars[i];
    return NULL;
}

Var *var_find(char *name) { return var_find_in(name, g_scope); }

static Var *var_create_in(char *name, int sc) {
    extern jmp_buf g_parse_error_jmp;
    extern int g_parse_error_active;
    char cb[MAX_VARNAME];
    name = (char *)var_canon(name, cb);

    /* a slot freed when a procedure returned, else a new one */
    Var *v = NULL;
    if (g_locals)
        for (int i = 0; i < g_nvar; i++)
            if (!g_vars[i].name) { v = &g_vars[i]; break; }
    if (!v) {
        if (g_nvar >= MAX_VARS) {
            basic_stderr("Too many variables %i/%i\n", g_nvar, MAX_VARS);
            if (g_parse_error_active) {
                longjmp(g_parse_error_jmp, 1);
            } else {
                exit(1);
            }
        }
        v = &g_vars[g_nvar++];
        /* A slot reused after RUN (which just resets the count) may still hold
         * the old variable's name, string or array. */
        free(v->name);
        if (v->kind == VAR_STR) free(v->str);
        var_free_arrays(v);
    }
    memset(v, 0, sizeof(*v));
    v->name = bstrdup(name);
    v->scope = scope_of(name, sc);
    if (var_is_str_name(name)) {
        v->kind = VAR_STR;
        v->str  = str_dup("");
    } else {
        v->kind = VAR_NUM;
        v->is_int = var_name_is_int(name);
        mpf_init2(v->num, g_prec);
        mpf_set_ui(v->num, 0);
    }
    return v;
}

Var *var_create(char *name) { return var_create_in(name, g_scope); }

Var *var_get_in(char *name, int sc) {
    Var *v = var_find_in(name, sc);
    return v ? v : var_create_in(name, sc);
}

Var *var_get(char *name) { return var_get_in(name, g_scope); }

/* A TYPE array's fields live in parallel arrays, one per field, named
 * BASE.FIELD (BASE.FIELD$ for a string field) and dimensioned like BASE --
 * not one variable per element per field, which ran out of variable slots
 * (and slowed every lookup) for any sizeable array of records. Null if
 * BASE(...).FIELD isn't stored that way. */
Var *field_array(const char *base, const char *field, int *is_str) {
    char n[MAX_VARNAME];
    snprintf(n, sizeof n, "%s.%s$", base, field);
    Var *v = var_find(n);
    if (v && v->kind == VAR_ARRAY_STR) { *is_str = 1; return v; }
    n[strlen(n) - 1] = '\0';
    v = var_find(n);
    if (v && v->kind == VAR_ARRAY_NUM) { *is_str = 0; return v; }
    return NULL;
}

/* ================================================================
 * Array element access (1-based or option-base-based indices)
 * ================================================================ */
/* Allocate an array of `total` elements (numbers set to 0, strings to "")
 * in place of whatever v held. False if there's no memory for it. */
bool var_alloc_array(Var *v, int total, int is_str) {
    var_free_arrays(v);
    if (total < 1) total = 1;
    if (is_str) {
        v->arr_str = (char **)calloc((size_t)total, sizeof(char *));
        if (!v->arr_str) return false;
        for (int i = 0; i < total; i++) v->arr_str[i] = str_dup("");
    } else {
        v->arr_num = (mpf_t *)calloc((size_t)total, sizeof(mpf_t));
        if (!v->arr_num) return false;
        for (int i = 0; i < total; i++) { mpf_init2(v->arr_num[i], g_prec); mpf_set_ui(v->arr_num[i], 0); }
    }
    v->arr_len = total;
    return true;
}

void var_free_arrays(Var *v) {
    if (v->arr_num) {
        for (int i = 0; i < v->arr_len; i++) mpf_clear(v->arr_num[i]);
        free(v->arr_num);
        v->arr_num = NULL;
    }
    if (v->arr_str) {
        for (int i = 0; i < v->arr_len; i++) free(v->arr_str[i]);
        free(v->arr_str);
        v->arr_str = NULL;
    }
    v->arr_len = 0;
}

/* Out-of-range indexes clamp (with a warning), as before; an array that
 * couldn't be allocated reads and writes a scratch element. */
mpf_t *arr_num_elem(Var *v, int i, int j) {
    static mpf_t scratch;
    static int scratch_ready = 0;
    int oi  = i - g_option_base;
    int oj  = j - g_option_base;
    int idx = (v->ndim == 2) ? (oi * v->dim[1] + oj) : oi;
    int total = v->arr_len;
    if (!v->arr_num || total < 1) {
        if (!scratch_ready) { mpf_init2(scratch, g_prec); scratch_ready = 1; }
        mpf_set_ui(scratch, 0);
        return &scratch;
    }
    if (idx < 0 || idx >= total) {
        basic_stderr("Array out of bounds: index %d (size %d) -- clamping\n", idx, total);
        idx = (idx < 0) ? 0 : total - 1;
    }
    return &v->arr_num[idx];
}

char **arr_str_elem(Var *v, int i, int j) {
    static char *scratch = NULL;
    int oi  = i - g_option_base;
    int oj  = j - g_option_base;
    int idx = (v->ndim == 2) ? (oi * v->dim[1] + oj) : oi;
    int total = v->arr_len;
    if (!v->arr_str || total < 1) {
        free(scratch);
        scratch = str_dup("");
        return &scratch;
    }
    if (idx < 0 || idx >= total) {
        basic_stderr("Array out of bounds: index %d (size %d) -- clamping\n", idx, total);
        idx = (idx < 0) ? 0 : total - 1;
    }
    return &v->arr_str[idx];
}

BASIC_NS_END
