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

Var *var_find(char *name) {
    for (int i = 0; i < g_nvar; i++)
        if (strcasecmp(g_vars[i].name, name) == 0) return &g_vars[i];
    return NULL;
}

Var *var_create(char *name) {
    extern jmp_buf g_parse_error_jmp;
    extern int g_parse_error_active;
    
    if (g_nvar >= MAX_VARS) {
        basic_stderr("Too many variables %i/%i\n", g_nvar, MAX_VARS);
        if (g_parse_error_active) {
            longjmp(g_parse_error_jmp, 1);
        } else {
            exit(1);
        }
    }
    Var *v = &g_vars[g_nvar++];
    /* A slot reused after RUN (which just resets the count) may still hold
     * the old variable's name, string or array. */
    free(v->name);
    if (v->kind == VAR_STR) free(v->str);
    var_free_arrays(v);
    memset(v, 0, sizeof(*v));
    v->name = bstrdup(name);
    if (var_is_str_name(name)) {
        v->kind = VAR_STR;
        v->str  = str_dup("");
    } else {
        v->kind = VAR_NUM;
        mpf_init2(v->num, g_prec);
        mpf_set_ui(v->num, 0);
    }
    return v;
}

Var *var_get(char *name) {
    Var *v = var_find(name);
    return v ? v : var_create(name);
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
