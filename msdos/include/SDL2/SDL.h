/* SDL2/SDL.h for the MS-DOS build: felixterminal's terminal, sixel and kitty
   code is written against SDL2; on DOS it runs on SDL3 (whose DOS port drives
   VESA, the keyboard and the mouse). This maps the SDL2 calls those files
   make onto SDL3. */
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

typedef SDL_Mutex SDL_mutex;

/* SDL2's SDL_RenderCopy/SDL_RenderCopyF: an integer source rect. */
static inline int SDL_RenderCopyF(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_FRect *dst)
{
    SDL_FRect s, *sp = NULL;
    if (src) { SDL_RectToFRect(src, &s); sp = &s; }
    return SDL_RenderTexture(r, t, sp, dst) ? 0 : -1;
}
static inline int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst)
{
    SDL_FRect d, *dp = NULL;
    if (dst) { SDL_RectToFRect(dst, &d); dp = &d; }
    return SDL_RenderCopyF(r, t, src, dp);
}
/* SDL2's SDL_RenderReadPixels fills a buffer; SDL3's returns a surface. */
static inline int SDL2_RenderReadPixels(SDL_Renderer *r, const SDL_Rect *rect, SDL_PixelFormat fmt, void *pixels, int pitch)
{
    SDL_Surface *s = SDL_RenderReadPixels(r, rect);
    if (!s) return -1;
    bool ok = SDL_ConvertPixels(s->w, s->h, s->format, s->pixels, s->pitch, fmt, pixels, pitch);
    SDL_DestroySurface(s);
    return ok ? 0 : -1;
}
#define SDL_RenderReadPixels SDL2_RenderReadPixels
#define SDL_SetClipboardText(t) ((void)(t), 0)
#define SDL_SetWindowTitle(w, t) ((void)(w), (void)(t))
#define SDL_CreateMutex() ((SDL_Mutex *)0)
#define SDL_LockMutex(m) ((void)(m))
#define SDL_UnlockMutex(m) ((void)(m))
#define SDL_DestroyMutex(m) ((void)(m))
