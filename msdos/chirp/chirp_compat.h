/* chirp_compat.h: forced into every file of the Felix Chirp DOS build
   (-include), after FLT's dos_compat.h (mutexes that never wait: DOS runs one
   thread). DJGPP's C library has no mkstemps (chirp_dos.cpp has one, for
   %TEMP%) and no fminf/fmaxf. */
#pragma once
#include "../dos_compat.h"
#ifdef __cplusplus
extern "C" {
#endif
int mkstemps(char *tmpl, int suffixlen);
#ifdef __cplusplus
}
#endif
static inline float fminf(float a, float b) { return a < b ? a : b; }
static inline float fmaxf(float a, float b) { return a > b ? a : b; }
