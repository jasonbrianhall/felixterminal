#pragma once
/* Force-included (-include) into every file of the DJGPP build: the few
 * POSIX bits DJGPP 2.05 lacks, and macros of its that clash with ours. */
#include <math.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>

#undef PI2                      /* DJGPP's math.h: M_PI_2; gfx code has a const */

#ifndef CLOCK_REALTIME
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
/* DJGPP's gettimeofday() only moves every BIOS tick (55 ms), so TIMER
 * would stand still across a short loop (Nibbles times FOR 1 TO 1000 to
 * set its speed, then divides by the difference). The wall clock is read
 * once; after that time comes from uclock(), the PIT, to the microsecond. */
inline int clock_gettime(int, struct timespec* ts) {
    static bool init;
    static struct timeval base_tv;
    static uclock_t base_uc;
    if (!init) {
        struct timeval a; gettimeofday(&a, 0);       /* start on a tick edge */
        do gettimeofday(&base_tv, 0); while (base_tv.tv_sec == a.tv_sec && base_tv.tv_usec == a.tv_usec);
        base_uc = uclock();
        init = true;
    }
    unsigned long long us = (unsigned long long)(uclock() - base_uc) * 1000000ull / UCLOCKS_PER_SEC
                          + (unsigned long long)base_tv.tv_usec;
    ts->tv_sec = base_tv.tv_sec + (time_t)(us / 1000000ull);
    ts->tv_nsec = (long)(us % 1000000ull) * 1000L;
    return 0;
}
#endif

#ifdef __cplusplus
static inline char* strcasestr(const char* h, const char* n) {
#else
static inline char* strcasestr(const char* h, const char* n) {
#endif
    size_t l = strlen(n);
    if (!l) return (char*)h;
    for (; *h; h++) if (strncasecmp(h, n, l) == 0) return (char*)h;
    return 0;
}

/* ../main.cpp's main() becomes felix_basic_main(); dos_platform.cpp has the
 * real one, which sets up the screen first. */
#ifndef FELIX_DOS_PLATFORM
#define main felix_basic_main
#endif
