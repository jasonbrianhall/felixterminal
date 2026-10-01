#pragma once
// Bare-metal stand-in for the few SDL2 pieces the interpreter core uses:
// integer types, the millisecond clock, delays and SDL_Log. The kernel
// (kernel.cpp) implements them; SDL_GetTicks and SDL_Delay also keep the
// keyboard, sound and screen serviced while a BASIC program runs.
#include <stdint.h>
#include <stddef.h>

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;
typedef struct SDL_semaphore SDL_sem;

#ifdef __cplusplus
extern "C" {
#endif
Uint32 SDL_GetTicks(void);
void   SDL_Delay(Uint32 ms);
void   SDL_Log(const char* fmt, ...);
#ifdef __cplusplus
}
#endif
