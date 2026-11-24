/* SDL2/SDL.h for Felix Chirp on MS-DOS: felixchirp/ is written against SDL2;
   on DOS it runs on SDL3 (whose DOS port drives VESA, the Sound Blaster, the
   keyboard and the mouse). This maps the SDL2 names Chirp uses onto SDL3. */
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

/* SDL3 spells the letter keys in capitals and the modifiers SDL_KMOD_ */
#define SDLK_a SDLK_A
#define SDLK_b SDLK_B
#define SDLK_c SDLK_C
#define SDLK_d SDLK_D
#define SDLK_e SDLK_E
#define SDLK_f SDLK_F
#define SDLK_g SDLK_G
#define SDLK_h SDLK_H
#define SDLK_i SDLK_I
#define SDLK_j SDLK_J
#define SDLK_k SDLK_K
#define SDLK_l SDLK_L
#define SDLK_m SDLK_M
#define SDLK_n SDLK_N
#define SDLK_o SDLK_O
#define SDLK_p SDLK_P
#define SDLK_q SDLK_Q
#define SDLK_r SDLK_R
#define SDLK_s SDLK_S
#define SDLK_t SDLK_T
#define SDLK_u SDLK_U
#define SDLK_v SDLK_V
#define SDLK_w SDLK_W
#define SDLK_x SDLK_X
#define SDLK_y SDLK_Y
#define SDLK_z SDLK_Z
#undef KMOD_SHIFT     /* (SDL_oldnames.h makes them point at the new names) */
#undef KMOD_CTRL
#undef KMOD_ALT
#define KMOD_SHIFT SDL_KMOD_SHIFT
#define KMOD_CTRL  SDL_KMOD_CTRL
#define KMOD_ALT   SDL_KMOD_ALT

/* SDL2's SDL_Vertex has an 8-bit SDL_Color; SDL3's an SDL_FColor (0..1). */
typedef struct SDL2_Vertex {
    SDL_FPoint position;
    SDL_Color  color;
    SDL_FPoint tex_coord;
} SDL2_Vertex;

/* Chirp draws every picture as two triangles. When they make an upright
   rectangle with the whole texture on it (the usual case: no rotation), this
   is a plain SDL_RenderTexture, which SDL's software renderer scales far
   faster than it maps triangles. Anything else goes to SDL_RenderGeometry. */
static inline int SDL2_RenderGeometry(SDL_Renderer *r, SDL_Texture *t, const SDL2_Vertex *v, int n,
                                      const int *idx, int nidx)
{
    if (t && n == 6 && !idx) {
        float x0 = v[0].position.x, y0 = v[0].position.y, x1 = x0, y1 = y0;
        int i, white = 1;
        for (i = 1; i < 6; i++) {
            if (v[i].position.x < x0) x0 = v[i].position.x;
            if (v[i].position.x > x1) x1 = v[i].position.x;
            if (v[i].position.y < y0) y0 = v[i].position.y;
            if (v[i].position.y > y1) y1 = v[i].position.y;
        }
        int upright = 1;
        for (i = 0; i < 6; i++) {
            const SDL2_Vertex *p = &v[i];
            if (p->color.r != 255 || p->color.g != 255 || p->color.b != 255 || p->color.a != 255) white = 0;
            float u = (p->position.x == x0) ? 0.f : (p->position.x == x1) ? 1.f : -1.f;
            float w = (p->position.y == y0) ? 0.f : (p->position.y == y1) ? 1.f : -1.f;
            if (u < 0 || w < 0 || p->tex_coord.x != u || p->tex_coord.y != w) upright = 0;
        }
        if (white && upright) {
            SDL_FRect d = { x0, y0, x1 - x0, y1 - y0 };
            return SDL_RenderTexture(r, t, NULL, &d) ? 0 : -1;
        }
    }
    SDL_Vertex buf[64];
    SDL_Vertex *out = n <= 64 ? buf : (SDL_Vertex *)SDL_malloc(sizeof(SDL_Vertex) * n);
    if (!out) return -1;
    for (int i = 0; i < n; i++) {
        out[i].position = v[i].position;
        out[i].color.r = v[i].color.r / 255.f;
        out[i].color.g = v[i].color.g / 255.f;
        out[i].color.b = v[i].color.b / 255.f;
        out[i].color.a = v[i].color.a / 255.f;
        out[i].tex_coord = v[i].tex_coord;
    }
    bool ok = SDL_RenderGeometry(r, t, out, n, idx, nidx);
    if (out != buf) SDL_free(out);
    return ok ? 0 : -1;
}
#define SDL_Vertex SDL2_Vertex
#define SDL_RenderGeometry SDL2_RenderGeometry

/* Clip rects, turned textures and a texture's size, as SDL2 spells them
   (SDL_oldnames.h points the old names at errors: they're replaced here). */
#undef SDL_RenderSetClipRect
#undef SDL_RenderCopyExF
#undef SDL_QueryTexture
static inline int SDL2_RenderSetClipRect(SDL_Renderer *r, const SDL_Rect *rect)
{
    return SDL_SetRenderClipRect(r, rect) ? 0 : -1;
}
static inline int SDL2_RenderCopyExF(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_FRect *dst,
                                     double angle, const SDL_FPoint *center, SDL_FlipMode flip)
{
    SDL_FRect s;
    if (src) { s.x = (float)src->x; s.y = (float)src->y; s.w = (float)src->w; s.h = (float)src->h; }
    return SDL_RenderTextureRotated(r, t, src ? &s : NULL, dst, angle, center, flip) ? 0 : -1;
}
static inline int SDL2_QueryTexture(SDL_Texture *t, Uint32 *format, int *access, int *w, int *h)
{
    float fw = 0.f, fh = 0.f;
    if (!SDL_GetTextureSize(t, &fw, &fh)) return -1;
    if (format) *format = 0;
    if (access) *access = 0;
    if (w) *w = (int)fw;
    if (h) *h = (int)fh;
    return 0;
}
#define SDL_RenderSetClipRect SDL2_RenderSetClipRect
#define SDL_RenderCopyExF SDL2_RenderCopyExF
#define SDL_QueryTexture SDL2_QueryTexture

typedef SDL_Mutex SDL_mutex;
#define SDL_CreateMutex() ((SDL_Mutex *)0)
#define SDL_LockMutex(m) ((void)(m))
#define SDL_UnlockMutex(m) ((void)(m))
#define SDL_DestroyMutex(m) ((void)(m))
