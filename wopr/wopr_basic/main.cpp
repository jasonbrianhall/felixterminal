/*
 * main.c Main interpreter loop, SIGINT handler, interactive REPL,
 *           and program entry point.
 */
#include "basic.h"
#include <setjmp.h>

#ifdef USE_SDL_WINDOW
#include "basic_gfx.h"
#include "basic_gfx_sdl.h"
#include "display.h"
#include "basic.h"       // g_break, BASIC_NS
#include "DejaVuMono.h"  // DEJAVU_REGULAR_FONT_B64 / _SIZE
#endif


#ifdef _WIN32
#include <windows.h>
#include <ctype.h>

static char *strcasestr(char *haystack, char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            char *h = haystack, *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}
#endif

#ifdef INLINEBASIC
#include "inline_basic.h"
#endif


#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

#if defined(WOPR) || defined(FELIX_BASIC)
    char                    BASIC_AUTOLOAD_SYM[DEFAULT_BUFFER] = {0};
    volatile sig_atomic_t   BASIC_BREAK_SYM         = 0;
#endif

BASIC_NS_BEGIN

/* Global precision variable */
#ifndef DONTUSEGMP
mp_bitcnt_t g_prec = DEFAULT_PREC;
#else
int g_prec = DEFAULT_PREC;
#endif

#if defined(WOPR) || defined(FELIX_BASIC)

char (&g_autoload_path)[DEFAULT_BUFFER] = ::BASIC_AUTOLOAD_SYM;
#endif


static void sigint_handler(int sig) {
    (void)sig;
    g_break = 1;
}

static void install_sigint(void) {
#ifndef _WIN32
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
#else
    signal(SIGINT, sigint_handler);
#endif
}

/* ================================================================
 * Typed program lines
 * ================================================================ */
/* Split a typed line at its top-level colons into one statement per
 * g_lines entry, the way load() stores a program read from a file: the
 * interpreter runs one statement per entry, so a typed
 * "10 A = 1: PRINT A" would otherwise run only "A = 1". REM, ' and IF
 * keep the rest of the line together, as in load(). */
static int typed_line_segments(char *p, char *segs[], int max) {
    int n = 0;
    while (isspace((unsigned char)*p)) p++;
    if ((strncasecmp(p, "IF", 2) == 0 && !isalnum((unsigned char)p[2]) && p[2] != '_') ||
        (strncasecmp(p, "REM", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_') ||
        *p == '\'') {
        segs[n++] = p;
        return n;
    }
    char *seg = p;
    int in_str = 0;
    for (char *c = p; *c; c++) {
        if (*c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (*c == '\'') { *c = '\0'; break; }
        if (*c != ':') continue;
        *c = '\0';
        if (*seg && n < max) segs[n++] = seg;
        seg = c + 1;
        while (isspace((unsigned char)*seg)) seg++;
        if ((strncasecmp(seg, "REM", 3) == 0 && !isalnum((unsigned char)seg[3]) && seg[3] != '_') || *seg == '\'') {
            seg = NULL;
            break;
        }
        if (strncasecmp(seg, "IF", 2) == 0 && !isalnum((unsigned char)seg[2]) && seg[2] != '_') break;
        c = seg - 1;
    }
    if (seg && *seg && n < max) segs[n++] = seg;
    return n;
}

/* ================================================================
 * Interpreter run loop
 * ================================================================ */
void run(void) { run_from(0); }

/* Print a BASIC-level stack trace to stderr */
static void print_stacktrace(void) {
    basic_stderr("BASIC stack trace (most recent first):\n");
    basic_stderr("  line %d: %s\n",
                 g_lines[g_current_pc].linenum,
                 g_lines[g_current_pc].text);
    /* Walk the control stack for GOSUB frames */
    for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
        if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0) {
            int ret_pc = g_ctrl[fi].line_idx - 1;
            if (ret_pc >= 0 && ret_pc < g_nlines)
                basic_stderr("  called from line %d: %s\n",
                             g_lines[ret_pc].linenum,
                             g_lines[ret_pc].text);
        }
    }
}

void run_from(int start_pc) {
    g_break = 0;
    Interp ip = { .pc = start_pc, .running = 1 };
    sound_init();


#ifdef USE_SDL_WINDOW
    Uint32 last_frame = SDL_GetTicks();

    while (ip.running && ip.pc < g_nlines && !g_break) {

        // Execute one BASIC instruction
        int old_pc = ip.pc;
        g_current_pc = old_pc;
        int jumped = dispatch(&ip, g_lines[ip.pc].text);

        // Render only every 16ms (~60 FPS), not every statement
        Uint32 now = SDL_GetTicks();
        if (now - last_frame >= 16) {
            // Pump SDL events
            if (!gfx_sdl_pump())
                break;
            // Render accumulated pixels
            gfx_sdl_render();
            last_frame = now;
        }

        if (g_tron) {
            char tbuf[32];
            snprintf(tbuf, sizeof tbuf, "[%d]", g_lines[old_pc].linenum);
            display_print(tbuf);
        }
        if (jumped < 0) {
            g_err = 255; g_erl = g_lines[old_pc].linenum;
            if (g_error_handler[0]) {
                g_error_resume_pc = old_pc;
                cmd_goto(&ip, g_error_handler);
                jumped = 1;
            }
        } else if (!jumped) {
            ip.pc = old_pc + 1;
        }
    }

    // Final frame
    gfx_sdl_pump();
    gfx_sdl_render();

#else
    while (ip.running && ip.pc < g_nlines && !g_break) {
        int old_pc = ip.pc;
        g_current_pc = old_pc;
        int jumped = dispatch(&ip, g_lines[ip.pc].text);

        if (g_tron) {
            char tbuf[32];
            snprintf(tbuf, sizeof tbuf, "[%d]", g_lines[old_pc].linenum);
            display_print(tbuf);
        }
        if (jumped < 0) {
            g_err = 255; g_erl = g_lines[old_pc].linenum;
            if (g_error_handler[0]) {
                g_error_resume_pc = old_pc;
                cmd_goto(&ip, g_error_handler);
                jumped = 1;
            }
        } else if (!jumped) {
            ip.pc = old_pc + 1;
        }
    }
#endif

    // Shared break handling
    if (g_break) {
        g_cont_pc = ip.pc;
        display_newline();
        display_print("Break\n");
        g_break = 0;
    }
}



#if defined(WOPR) || defined(FELIX_BASIC)
int basic_main(void) {
#else
int basic_main(int argc, char **argv) {
#endif
    g_prec = DEFAULT_PREC;
    mpf_set_default_prec(g_prec);
    clear_program();   /* flush g_nlines/g_nvar/etc. left over from a prior session */
    srand((unsigned)time(NULL));
    display_init();
    install_sigint();

    /* Direct run mode: ./basic program.bas [precision_bits] */
#ifndef INLINEBASIC
#if !defined(WOPR) && !defined(FELIX_BASIC)
    if (argc >= 2) {
        if (argc >= 3) g_prec = (mp_bitcnt_t)atoi(argv[2]);
        load(argv[1]);
        prescan_data();
        run();
        display_shutdown();
        return 0;
    }
#endif
#else
char tmpfile_path[DEFAULT_BUFFER];

#ifdef _WIN32
    // Windows: use GetTempPath + GetTempFileName
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    GetTempFileNameA(tmpdir, "WOP", 0, tmpfile_path);

    FILE *dbfile = fopen(tmpfile_path, "wb");
    if (!dbfile) {
        basic_stderr("Could not open temp file\n");
        return 1;
    }

    fwrite(wopr_basic_program, 1, wopr_basic_program_len, dbfile);
    fclose(dbfile);

#else
    // POSIX: use mkstemp
    strcpy(tmpfile_path, "/tmp/woprXXXXXX");
    int fd = mkstemp(tmpfile_path);
    if (fd == -1) {
        perror("mkstemp");
        return 1;
    }

    write(fd, wopr_basic_program, wopr_basic_program_len);
    close(fd);
#endif

// NOW replace argv[1] with the filename
argv[1] = tmpfile_path;

load(argv[1]);
prescan_data();
run();
display_shutdown();
return 0;
#endif

#if defined(WOPR) || defined(FELIX_BASIC)
    /* Auto-load a file if one was set before the thread started */
    if (g_autoload_path[0]) {
        load_program(g_autoload_path);
        prescan_data();
        run();
        display_shutdown();
        return 0;
    }
#endif

    /* ----------------------------------------------------------------
     * REPL helper macros
     * ---------------------------------------------------------------- */
    #define PARSE_FILENAME(dst, src) do { \
        char *_q = sk(src); \
        if (*_q == '"') _q++; \
        int _i = 0; \
        while (*_q && *_q != '"' && *_q != ',' && !isspace((unsigned char)*_q) && _i < 255) \
            (dst)[_i++] = *_q++; \
        (dst)[_i] = '\0'; \
        if (*_q == '"') _q++; \
        (src) = _q; \
    } while(0)

    #define BAS_EXT(dst, src) do { \
        if (strchr((src), '.')) snprintf((dst), sizeof(dst), "%s", (src)); \
        else snprintf((dst), sizeof(dst), "%s.bas", (src)); \
    } while(0)

    /* Expand leading ~/ to $HOME/ */
    #define EXPAND_TILDE(buf) do { \
        if ((buf)[0] == '~' && ((buf)[1] == '/' || (buf)[1] == '\0')) { \
            const char *_home = getenv("HOME"); \
            if (_home) { \
                char _tmp[DEFAULT_BUFFER]; \
                snprintf(_tmp, sizeof(_tmp), "%s%s", _home, (buf) + 1); \
                strncpy((buf), _tmp, sizeof(buf) - 1); \
                (buf)[sizeof(buf) - 1] = '\0'; \
            } \
        } \
    } while(0)

    /* ----------------------------------------------------------------
     * Interactive REPL
     * ---------------------------------------------------------------- */
    display_cls();
    display_print("Felix BASIC\n");
    display_print("In loving memory of Felix the lovebird.\n");
    display_print("Type HELP for a list of commands, or SYSTEM to exit.\n\n");

    char line[DEFAULT_BUFFER];
    int suppress_ok = 0;
    for (;;) {
        if (!suppress_ok) display_print("\nOk\n");
        suppress_ok = 0;
        display_cursor(1);
        int _gl_ret = display_getline(line, sizeof line);
        if (_gl_ret < 0) break;   // window closed (SDL_QUIT)
        if (!_gl_ret) {
#if defined(WOPR) || defined(FELIX_BASIC)
            break;  /* \00 kill-switch from host */
#else
            continue;  /* bare Enter in CLI just loop */
#endif
        }
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        /* SYSTEM / EXIT / QUIT / BYE */
        if (strncasecmp(p,"SYSTEM",6)==0 || strncasecmp(p,"EXIT",4)==0 ||
            strncasecmp(p,"QUIT",4)==0   || strncasecmp(p,"BYE",3)==0) {
            break;

        } else if (strncasecmp(p,"NEW",3)==0 && !isalnum((unsigned char)p[3])) {
            clear_program();
            display_print("\nOk\n");

        } else if (strncasecmp(p,"PWD",3)==0 && !isalnum((unsigned char)p[3])) {
            char cwd[DEFAULT_BUFFER];
            if (getcwd(cwd, sizeof cwd)) { char out[DEFAULT_BUFFER+2]; snprintf(out, sizeof out, "%s\n", cwd); display_print(out); }
            else perror("getcwd");

        } else if (strncasecmp(p,"FILES",5)==0 || strncasecmp(p,"DIR",3)==0 || strncasecmp(p,"LS",2)==0) {
            int pfxlen = strncasecmp(p,"FILES",5)==0 ? 5 : (strncasecmp(p,"DIR",3)==0 ? 3 : 2);
            char *pat = sk(p + pfxlen);
            char filter[64] = ".bas";
            if (*pat == '"') pat++;
            if (*pat && *pat != '"') { strncpy(filter, pat, 63); char *eq=strchr(filter,'"'); if(eq)*eq='\0'; }
            { char cwd[DEFAULT_BUFFER]; if (getcwd(cwd, sizeof cwd)) { char hdr[DEFAULT_BUFFER+16]; snprintf(hdr, sizeof hdr, "  Directory: %s\n\n", cwd); display_print(hdr); } }
            DIR *d = opendir(".");
            if (!d) { perror("opendir"); continue; }

            /* Collect entries into two sorted lists: dirs and files */
            #ifndef MAX_DIR_ENTRIES
            #define MAX_DIR_ENTRIES 2048
            #endif
            // static, not stack: these are 512KB each. As plain locals they get
            // reserved as part of basic_main()'s single stack frame the moment the
            // function is entered (even if FILES/DIR/LS is never typed), and under
            // ASan's no-stack-slot-reuse instrumentation they can't share space with
            // any other branch's locals either. basic_main() runs once per thread,
            // never recursively/concurrently, so static here is safe.
            static char dirs[MAX_DIR_ENTRIES][256];
            static char files[MAX_DIR_ENTRIES][256];
            int ndirs = 0, nfiles = 0;

            struct dirent *de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.') continue;
                char *nm = de->d_name;
                struct stat st; bool is_dir = false;
                if (stat(nm, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
                if (is_dir) {
                    if (ndirs < MAX_DIR_ENTRIES)
                        strncpy(dirs[ndirs++], nm, 255);
                } else {
                    size_t nl = strlen(nm), fl = strlen(filter);
                    if (fl && (nl < fl || strcasecmp(nm + nl - fl, filter) != 0)) continue;
                    if (nfiles < MAX_DIR_ENTRIES)
                        strncpy(files[nfiles++], nm, 255);
                }
            }
            closedir(d);

            /* Sort each list alphabetically */
            auto cmp_str = [](const void *a, const void *b) {
                return strcasecmp((char *)a, (char *)b);
            };
            qsort(dirs,  ndirs,  sizeof dirs[0],  cmp_str);
            qsort(files, nfiles, sizeof files[0], cmp_str);

            /* Print dirs first, then files */
            for (int i = 0; i < ndirs;  i++) { char e[272]; snprintf(e, sizeof e, "  %-19s/\n", dirs[i]);  display_print(e); }
            for (int i = 0; i < nfiles; i++) { char e[272]; snprintf(e, sizeof e, "  %-20s\n",  files[i]); display_print(e); }

            if (!ndirs && !nfiles) display_print("  (no matching files)\n");
            #undef MAX_DIR_ENTRIES

        } else if (strncasecmp(p,"KILL",4)==0 && !isalnum((unsigned char)p[4])) {
            p += 4;
            char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p); EXPAND_TILDE(name);
            char path[DEFAULT_BUFFER]; BAS_EXT(path, name);
            if (remove(path) == 0) printf("Deleted %s\n", path); else perror(path);

        } else if (strncasecmp(p,"RENAME",6)==0 && !isalnum((unsigned char)p[6])) {
            p += 6;
            char old[DEFAULT_BUFFER], neo[DEFAULT_BUFFER];
            PARSE_FILENAME(old, p); EXPAND_TILDE(old); p = sk(p); if (*p==',') p++;
            PARSE_FILENAME(neo, p); EXPAND_TILDE(neo);
            char op[DEFAULT_BUFFER], np[DEFAULT_BUFFER]; BAS_EXT(op,old); BAS_EXT(np,neo);
            if (rename(op, np) == 0) printf("Renamed %s -> %s\n", op, np); else perror(op);

        } else if (strncasecmp(p,"CHDIR",5)==0 || strncasecmp(p,"CD",2)==0) {
            p += strncasecmp(p,"CHDIR",5)==0 ? 5 : 2;
            char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p); EXPAND_TILDE(name);
            if (!*name) { char cwd[DEFAULT_BUFFER]; if(getcwd(cwd,sizeof cwd)) printf("%s\n",cwd); }
            else if (chdir(name)!=0) perror(name);
            else { char cwd[DEFAULT_BUFFER]; if(getcwd(cwd,sizeof cwd)) printf("%s\n",cwd); }

        } else if (strncasecmp(p,"MKDIR",5)==0 && !isalnum((unsigned char)p[5])) {
            p += 5; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p); EXPAND_TILDE(name);
            if (
#ifdef _WIN32
                mkdir(name)
#else
                mkdir(name, 0755)
#endif
                != 0) perror(name); else printf("Created %s\n", name);

        } else if (strncasecmp(p,"RMDIR",5)==0 && !isalnum((unsigned char)p[5])) {
            p += 5; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p); EXPAND_TILDE(name);
            if (rmdir(name)!=0) perror(name); else printf("Removed %s\n", name);

        } else if (strncasecmp(p,"LOAD",4)==0 && !isalnum((unsigned char)p[4])) {
            p += 4; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p); EXPAND_TILDE(name);
            if (!*name) { display_print("Usage: LOAD \"filename\"\n"); continue; }
            load_program(name);

        } else if (strncasecmp(p,"SAVE",4)==0 && !isalnum((unsigned char)p[4])) {
            p += 4; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p); EXPAND_TILDE(name);
            if (!*name) { display_print("Usage: SAVE \"filename\"\n"); continue; }
            save_program(name);

        } else if (strncasecmp(p,"RUN",3)==0 && !isalnum((unsigned char)p[3])) {
            if (g_nlines == 0) { display_print("No program loaded.\n"); continue; }
            /* Clean up old variables before running */
            for (int i = 0; i < g_nvar; i++) {
                Var *v = &g_vars[i];
                if (v->kind == VAR_STR && v->str) { free(v->str); v->str = nullptr; }
                else if (v->kind == VAR_ARRAY_NUM) {
                    /* Arrays are now static in Var struct, just clear the mpf_t values */
                    int size = 0;
                    if (v->ndim == 1) size = v->dim[0];
                    else if (v->ndim == 2) size = v->dim[0] * v->dim[1];
                    else if (v->ndim > 0 && v->dim[0] > 0) size = v->dim[0] * (v->dim[1] > 0 ? v->dim[1] : 1);
                    for (int j = 0; j < size; j++) mpf_clear(v->arr_num[j]);
                }
                else if (v->kind == VAR_ARRAY_STR) {
                    /* Arrays are now static, just free the individual string elements */
                    int size = 0;
                    if (v->ndim == 1) size = v->dim[0];
                    else if (v->ndim == 2) size = v->dim[0] * v->dim[1];
                    else if (v->ndim > 0 && v->dim[0] > 0) size = v->dim[0] * (v->dim[1] > 0 ? v->dim[1] : 1);
                    for (int j = 0; j < size; j++) if (v->arr_str[j]) { free(v->arr_str[j]); v->arr_str[j] = nullptr; }
                }
                mpf_clear(v->num);
            }
            g_nvar = 0; g_ctrl_top = 0; g_data_pos = 0;
            run();

        } else if (strncasecmp(p,"CONT",4)==0 && !isalnum((unsigned char)p[4])) {
            if (g_cont_pc < 0) { display_print("Can't continue\n"); continue; }
            run_from(g_cont_pc);

        } else if (strncasecmp(p,"LIST",4)==0 && !isalnum((unsigned char)p[4])) {
            p = sk(p + 4);
            int from = 0, to = 999999;
            if (isdigit((unsigned char)*p)) {
                from = atoi(p);
                while (isdigit((unsigned char)*p)) p++;
                to = 999999; p = sk(p);
                if (*p == '-') { p=sk(p+1); to=isdigit((unsigned char)*p)?atoi(p):999999; }
                else if (*p == ',') { p=sk(p+1); to=isdigit((unsigned char)*p)?atoi(p):999999; }
                else { to = 999999; }
            }
            int prev = -1;
            for (int i = 0; i < g_nlines; i++) {
                int ln = g_lines[i].linenum;
                if (ln < from || ln > to) continue;
                if (ln != prev) { if (prev != -1) display_print("\n"); char hdr[32]; snprintf(hdr,sizeof hdr,"%d ",ln); display_print(hdr); prev = ln; }
                else display_print(": ");
                // Strip trailing newline from line text since we control newlines here
                char *text = g_lines[i].text;
                int len = strlen(text);
                if (len > 0 && text[len-1] == '\n') {
                    char buf[MAX_LINE_LEN];
                    strncpy(buf, text, len-1);
                    buf[len-1] = '\0';
                    display_print(buf);
                } else {
                    display_print(text);
                }
            }
            if (prev != -1) display_print("\n");

        } else if (strncasecmp(p,"DELETE",6)==0 && !isalnum((unsigned char)p[6])) {
            p = sk(p+6);
            if (!isdigit((unsigned char)*p)) { display_print("Usage: DELETE start[-end]\n"); continue; }
            int from = atoi(p); while (isdigit((unsigned char)*p)) p++;
            int to = from; p = sk(p);
            if (*p == '-') { p=sk(p+1); to=isdigit((unsigned char)*p)?atoi(p):999999; }
            int w = 0;
            for (int i = 0; i < g_nlines; i++) {
                if (g_lines[i].linenum < from || g_lines[i].linenum > to) g_lines[w++] = g_lines[i];
                else free(g_lines[i].text);
            }
            printf("Deleted %d entries.\n", g_nlines - w);
            g_nlines = w;

        } else if (strncasecmp(p,"RENUM",5)==0 && !isalnum((unsigned char)p[5])) {
            p = sk(p+5);
            int new_start=10, step=10;
            if (isdigit((unsigned char)*p)) { new_start=atoi(p); while(isdigit((unsigned char)*p))p++; }
            p=sk(p); if(*p==',') p=sk(p+1);
            if (isdigit((unsigned char)*p)) { while(isdigit((unsigned char)*p))p++; }
            p=sk(p); if(*p==',') p=sk(p+1);
            if (isdigit((unsigned char)*p)) { step=atoi(p); }
            if (step < 1) step = 10;
            int old_nums[MAX_LINES], new_nums[MAX_LINES], nmap = 0, prev2 = -1;
            for (int i = 0; i < g_nlines; i++) {
                if (g_lines[i].linenum != prev2) {
                    old_nums[nmap] = g_lines[i].linenum;
                    new_nums[nmap] = new_start + nmap * step;
                    nmap++; prev2 = g_lines[i].linenum;
                }
            }
            for (int i = 0; i < g_nlines; i++) {
                for (int j = 0; j < nmap; j++) {
                    if (g_lines[i].linenum == old_nums[j]) { g_lines[i].linenum = new_nums[j]; break; }
                }
                char *kws[] = {"GOTO","GOSUB","THEN","ELSE","RESTORE","RUN", NULL};
                for (int k = 0; kws[k]; k++) {
                    char *kw = strcasestr(g_lines[i].text, kws[k]);
                    if (!kw) continue;
                    char *np2 = kw + strlen(kws[k]);
                    while (isspace((unsigned char)*np2)) np2++;
                    if (!isdigit((unsigned char)*np2)) continue;
                    int old_target = atoi(np2);
                    for (int j = 0; j < nmap; j++) {
                        if (old_nums[j] == old_target) {
                            char newnum[16]; snprintf(newnum,sizeof newnum,"%d",new_nums[j]);
                            char *end_num = np2;
                            while (isdigit((unsigned char)*end_num)) end_num++;
                            /* Build the full replacement line into a temp buffer —
                               text is no longer a fixed MAX_LINE_LEN buffer with
                               headroom, so we can't grow it in place. */
                            size_t prefix_len = (size_t)(np2 - g_lines[i].text);
                            size_t total = prefix_len + strlen(newnum) + strlen(end_num) + 1;
                            char *rebuilt = (char*)malloc(total);
                            memcpy(rebuilt, g_lines[i].text, prefix_len);
                            snprintf(rebuilt + prefix_len, total - prefix_len, "%s%s", newnum, end_num);
                            free(g_lines[i].text);
                            g_lines[i].text = rebuilt;
                            break;
                        }
                    }
                }
            }
            qsort(g_lines, g_nlines, sizeof(Line), line_cmp);
            printf("Renumbered %d logical lines starting at %d step %d.\n", nmap, new_start, step);

        } else if (strncasecmp(p,"HELP",4)==0) {
            display_print(
                "Felix BASIC\n"
                "In loving memory of Felix the lovebird.\n"
                "\n"
                "Command Reference\n"
                "--------------------------------\n"
                "PROGRAM CONTROL\n"
                "  NEW              Clear the current program\n"
                "  LOAD \"file\"      Load a .bas file\n"
                "  SAVE \"file\"      Save the current program\n"
                "  RUN              Run the loaded program\n"
                "  CONT             Continue after a break\n"
                "  LIST [n[-m]]     List program lines (optionally range n to m)\n"
                "  DELETE n[-m]     Delete program line(s)\n"
                "  RENUM [s[,i]]    Renumber lines (start, step)\n"
                "\n"
                "FILE MANAGEMENT\n"
                "  FILES / DIR / LS [\"pat\"]    List files in current directory\n"
                "  KILL \"file\"                 Delete a file\n"
                "  RENAME \"old\",\"new\"          Rename a file\n"
                "\n"
                "DIRECTORY NAVIGATION\n"
                "  PWD              Print working directory\n"
                "  CD / CHDIR \"path\"   Change directory\n"
                "  MKDIR \"path\"     Create a directory\n"
                "  RMDIR \"path\"     Remove a directory\n"
                "\n"
                "SYSTEM\n"
                "  SYSTEM / EXIT / QUIT / BYE   Exit Felix BASIC\n"
            );

        } else if (isdigit((unsigned char)*p)) {
            /* Distinguish a numbered line (10 PRINT "hi") from a bare
             * expression typed in immediate mode (8+8*3, 2^10, etc.).
             * A line number is always digits followed by whitespace then
             * a statement.  If an operator immediately follows the digits
             * (no space, or space then operator), treat it as an expression. */
            {
                char *scan = p;
                while (isdigit((unsigned char)*scan)) scan++;
                char *after_digits = scan;
                while (isspace((unsigned char)*scan)) scan++;
                int is_expr = (*after_digits != '\00' && !isspace((unsigned char)*after_digits))
                              ? strchr("+-*/^\%.", *after_digits) != NULL
                              : (*scan == '+' || *scan == '-' || *scan == '*' ||
                                 *scan == '/' || *scan == '^' || *scan == '%' ||
                                 *scan == '=' || *scan == '<' || *scan == '>');
                if (is_expr) {
                    Interp ip2 = { .pc=0, .running=1 };
                    extern jmp_buf g_parse_error_jmp;
                    extern int g_parse_error_active;
                    g_parse_error_active = 1;
                    if (setjmp(g_parse_error_jmp) == 0) {
                        dispatch_one(&ip2, p, p);
                    }
                    g_parse_error_active = 0;
                    continue;
                }
            }
            /* Numbered line: add/replace in program store */
            int num = (int)strtol(p, (char **)&p, 10);
            while (isspace((unsigned char)*p)) p++;
            int w = 0;
            for (int i = 0; i < g_nlines; i++) {
                if (g_lines[i].linenum != num) g_lines[w++] = g_lines[i];
                else free(g_lines[i].text);
            }
            g_nlines = w;
            if (*p) {
                char *segs[256];
                int nseg = typed_line_segments(p, segs, 256);
                if (g_nlines + nseg > MAX_LINES) { display_print("Too many lines\n"); continue; }
                int ins = g_nlines;
                for (int i = 0; i < g_nlines; i++)
                    if (g_lines[i].linenum > num) { ins = i; break; }
                memmove(&g_lines[ins+nseg], &g_lines[ins], (g_nlines-ins)*sizeof(Line));
                for (int k = 0; k < nseg; k++) {
                    g_lines[ins+k].linenum = num;
                    g_lines[ins+k].text = bstrdup(segs[k]);
                }
                g_nlines += nseg;
            }
            suppress_ok = 1; continue;  /* no Ok prompt */

        } else {
            /* Immediate execution (every statement of "COLOR 14, 1: CLS") */
            Interp ip = { .pc=0, .running=1 };
            extern jmp_buf g_parse_error_jmp;
            extern int g_parse_error_active;
            g_parse_error_active = 1;
            if (setjmp(g_parse_error_jmp) == 0) {
                dispatch_multi(&ip, p);
            }
            g_parse_error_active = 0;
            continue;
        }
    }

    display_shutdown();
    clear_program();  /* Clean up all variables, arrays, and data before exit */
    return 0;
}

BASIC_NS_END

/* ================================================================
 * C-linkage entry points outside the namespace.
 *
 *  WOPR build:         extern "C" int basic_main(void)
 *  FELIX_BASIC build:  extern "C" int fb_basic_main(void)
 *  Standalone:         int main(int argc, char **argv)
 * ================================================================ */
#if defined(WOPR) || defined(FELIX_BASIC)
int basic_main(void) {
    return BASIC_NS::basic_main();
}
#else
int main(int argc, char **argv) {
    return BASIC_NS::basic_main(argc, argv);
}
#endif
