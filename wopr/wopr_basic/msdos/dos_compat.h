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
static inline int clock_gettime(int, struct timespec* ts) {
    struct timeval tv; gettimeofday(&tv, 0);
    ts->tv_sec = tv.tv_sec; ts->tv_nsec = tv.tv_usec * 1000L;
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
