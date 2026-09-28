#include "glyph_atlas.h"
#include FT_BITMAP_H      // FT_Bitmap_Convert
#include "ft_font.h"      // s_emoji_face
#include "sdl_renderer.h" // g_use_sdl_renderer
#include <SDL2/SDL.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

GlyphAtlas g_atlas;

// ============================================================================
// INIT / CLEAR / DESTROY
// ============================================================================

void GlyphAtlas::init() {
    // Already initialized for the correct path
    if (!g_use_sdl_renderer && tex)    return;
    if ( g_use_sdl_renderer && sdl_tex) return;

    // CPU pixel mirror — always allocated, used by both GL and SDL paths
    pixels.assign((size_t)ATLAS_W * ATLAS_H * 4, 0);

    if (!g_use_sdl_renderer) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_W, ATLAS_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        // Use STREAMING access — SDL's opengl backend handles this most reliably
        // for textures used as geometry sources while rendering to another target.
        sdl_tex = SDL_CreateTexture(g_sdl_renderer,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, ATLAS_W, ATLAS_H);
        if (sdl_tex) {
            SDL_SetTextureBlendMode(sdl_tex, SDL_BLENDMODE_BLEND);
            // Initialize to transparent
            void *px; int pitch;
            if (SDL_LockTexture(sdl_tex, nullptr, &px, &pitch) == 0) {
                memset(px, 0, pitch * ATLAS_H);
                SDL_UnlockTexture(sdl_tex);
            }
            SDL_Log("[Atlas] SDL texture created %dx%d\n", ATLAS_W, ATLAS_H);
        } else {
            SDL_Log("[Atlas] SDL texture creation FAILED: %s\n", SDL_GetError());
        }
    }

    cursor_x = ATLAS_PADDING;
    cursor_y = ATLAS_PADDING;
    row_h    = 0;
    cache.clear();
    SDL_Log("[Atlas] created %dx%d\n", ATLAS_W, ATLAS_H);
}

void GlyphAtlas::clear() {
    if (!tex && !sdl_tex) { init(); return; }

    pixels.assign((size_t)ATLAS_W * ATLAS_H * 4, 0);

    if (!g_use_sdl_renderer && tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS_W, ATLAS_H,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    } else if (sdl_tex) {
        void *px; int pitch;
        if (SDL_LockTexture(sdl_tex, nullptr, &px, &pitch) == 0) {
            // pitch may differ from ATLAS_W*4 — copy row by row
            for (int row = 0; row < ATLAS_H; row++)
                memset((uint8_t*)px + row * pitch, 0, ATLAS_W * 4);
            SDL_UnlockTexture(sdl_tex);
        }
    }

    cursor_x = ATLAS_PADDING;
    cursor_y = ATLAS_PADDING;
    row_h    = 0;
    generation++;
    cache.clear();
    SDL_Log("[Atlas] cleared\n");
}

void GlyphAtlas::destroy() {
    if (tex)     { glDeleteTextures(1, &tex); tex = 0; }
    if (sdl_tex) { SDL_DestroyTexture(sdl_tex); sdl_tex = nullptr; }
    pixels.clear();
    cache.clear();
}

// ============================================================================
// SHELF PACKER
// ============================================================================

static bool atlas_alloc(GlyphAtlas &a, int w, int h, int *out_x, int *out_y) {
    int pw = w + ATLAS_PADDING;
    int ph = h + ATLAS_PADDING;

    if (a.cursor_x + pw > ATLAS_W) {
        a.cursor_x  = ATLAS_PADDING;
        a.cursor_y += a.row_h + ATLAS_PADDING;
        a.row_h     = 0;
    }
    if (a.cursor_y + ph > ATLAS_H) {
        SDL_Log("[Atlas] FULL — evicting\n");
        a.clear();
    }

    *out_x = a.cursor_x;
    *out_y = a.cursor_y;
    a.cursor_x += pw;
    if (h > a.row_h) a.row_h = h;
    return true;
}

// ============================================================================
// GLYPH RASTERIZATION + UPLOAD
// ============================================================================

const GlyphEntry *GlyphAtlas::get(FT_Face face, uint32_t cp, int font_px, int emoji_px) {
    GlyphKey key { cp, face, font_px };
    auto it = cache.find(key);
    if (it != cache.end()) return &it->second;

    // ── Set face size ────────────────────────────────────────────────────────
    float glyph_scale = 1.0f;
    float cell_offset_x = 0.f, cell_offset_y = 0.f;

    const bool strikes = ft_face_uses_strikes(face);
    if (strikes) {
        int best = 0;
        int best_diff = abs(face->available_sizes[0].height - emoji_px);
        for (int si = 1; si < face->num_fixed_sizes; si++) {
            int diff = abs(face->available_sizes[si].height - emoji_px);
            if (diff < best_diff) { best_diff = diff; best = si; }
        }
        FT_Select_Size(face, best);
        glyph_scale = (float)emoji_px / (float)face->available_sizes[best].height;
    } else {
        int req_px = (face == s_emoji_face) ? emoji_px : font_px;
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)req_px);
    }

    int load_flags = FT_LOAD_RENDER;
    if (strikes || face == s_emoji_face)
        load_flags |= FT_LOAD_COLOR;
    else if (FT_IS_SCALABLE(face))
        load_flags |= FT_LOAD_NO_BITMAP;   // skip embedded 1-bit strikes, render outlines

    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (!gi) return nullptr;
    if (FT_Load_Glyph(face, gi, load_flags) != 0) return nullptr;

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap   *bm   = &slot->bitmap;

    if (bm->width == 0 || bm->rows == 0) {
        GlyphEntry e {};
        e.advance      = (int)(slot->advance.x >> 6);
        e.render_scale = glyph_scale;
        e.color        = false;
        cache[key] = e;
        return &cache[key];
    }

    // Bitmap-only fonts can still hand back 1/2/4-bit glyphs. Convert them to
    // 8-bit coverage so the copy loops below (which read one byte per pixel)
    // don't misread packed bits as garbage.
    FT_Bitmap converted;
    FT_Bitmap_Init(&converted);
    if (bm->pixel_mode == FT_PIXEL_MODE_MONO  ||
        bm->pixel_mode == FT_PIXEL_MODE_GRAY2 ||
        bm->pixel_mode == FT_PIXEL_MODE_GRAY4) {
        if (FT_Bitmap_Convert(slot->library, bm, &converted, 1) == 0) {
            int levels = converted.num_grays > 1 ? converted.num_grays - 1 : 1;
            for (unsigned r = 0; r < converted.rows; r++) {
                uint8_t *row = converted.buffer + r * converted.pitch;
                for (unsigned c = 0; c < converted.width; c++)
                    row[c] = (uint8_t)(row[c] * 255 / levels);
            }
            bm = &converted;
        }
    }

    int dst_w = (int)ceilf(bm->width  * glyph_scale);
    int dst_h = (int)ceilf(bm->rows   * glyph_scale);

    int ax, ay;
    atlas_alloc(*this, dst_w, dst_h, &ax, &ay);

    bool is_color = (bm->pixel_mode == FT_PIXEL_MODE_BGRA);

    // ── Build RGBA pixels and write into CPU mirror ───────────────────────────
    if (glyph_scale == 1.0f) {
        if (is_color) {
            for (int row = 0; row < (int)bm->rows; row++) {
                for (int col = 0; col < (int)bm->width; col++) {
                    const uint8_t *src = bm->buffer + row * bm->pitch + col * 4;
                    uint8_t *dst = pixels.data() + ((ay + row) * ATLAS_W + (ax + col)) * 4;
                    dst[0] = src[2]; // R ← B
                    dst[1] = src[1]; // G
                    dst[2] = src[0]; // B ← R
                    dst[3] = src[3]; // A
                }
            }
        } else {
            for (int row = 0; row < (int)bm->rows; row++) {
                for (int col = 0; col < (int)bm->width; col++) {
                    uint8_t pv = bm->buffer[row * bm->pitch + col];
                    uint8_t *dst = pixels.data() + ((ay + row) * ATLAS_W + (ax + col)) * 4;
                    uint8_t alpha = (uint8_t)(sqrtf(pv / 255.f) * 255.f);
                    // GL shader reads .r as coverage mask (tints via uniform color).
                    // SDL multiplies vertex_color * tex_color per channel, so store
                    // white (255,255,255) with alpha=coverage so tint color passes through.
                    dst[0] = 255;   // R — white so GL .r path and SDL multiply both work
                    dst[1] = 255;   // G
                    dst[2] = 255;   // B
                    dst[3] = alpha; // A — coverage
                }
            }
        }
    } else {
        float inv_scale = 1.0f / glyph_scale;
        if (is_color) {
            for (int row = 0; row < dst_h; row++) {
                int src_row = (int)(row * inv_scale);
                if (src_row >= (int)bm->rows) src_row = (int)bm->rows - 1;
                for (int col = 0; col < dst_w; col++) {
                    int src_col = (int)(col * inv_scale);
                    if (src_col >= (int)bm->width) src_col = (int)bm->width - 1;
                    const uint8_t *src = bm->buffer + src_row * bm->pitch + src_col * 4;
                    uint8_t *dst = pixels.data() + ((ay + row) * ATLAS_W + (ax + col)) * 4;
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = src[3];
                }
            }
        } else {
            for (int row = 0; row < dst_h; row++) {
                int src_row = (int)(row * inv_scale);
                if (src_row >= (int)bm->rows) src_row = (int)bm->rows - 1;
                for (int col = 0; col < dst_w; col++) {
                    int src_col = (int)(col * inv_scale);
                    if (src_col >= (int)bm->width) src_col = (int)bm->width - 1;
                    uint8_t pv = bm->buffer[src_row * bm->pitch + src_col];
                    uint8_t *dst = pixels.data() + ((ay + row) * ATLAS_W + (ax + col)) * 4;
                    uint8_t alpha = (uint8_t)(sqrtf(pv / 255.f) * 255.f);
                    dst[0] = 255;
                    dst[1] = 255;
                    dst[2] = 255;
                    dst[3] = alpha;
                }
            }
        }
        cell_offset_x = (font_px - dst_w) * 0.5f;
        cell_offset_y = 0.f;
    }

    // ── Upload dirty rect to GPU ──────────────────────────────────────────────
    if (!g_use_sdl_renderer && tex) {
        // Upload only the dirty rect for this glyph
        std::vector<uint8_t> rect(dst_w * dst_h * 4);
        for (int row = 0; row < dst_h; row++)
            memcpy(rect.data() + row * dst_w * 4,
                   pixels.data() + ((ay + row) * ATLAS_W + ax) * 4,
                   dst_w * 4);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, dst_w, dst_h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rect.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    } else if (sdl_tex) {
        // Lock just the dirty rect and copy from CPU mirror
        SDL_Rect r = { ax, ay, dst_w, dst_h };
        void *px; int pitch;
        if (SDL_LockTexture(sdl_tex, &r, &px, &pitch) == 0) {
            for (int row = 0; row < dst_h; row++)
                memcpy((uint8_t*)px + row * pitch,
                       pixels.data() + ((ay + row) * ATLAS_W + ax) * 4,
                       dst_w * 4);
            SDL_UnlockTexture(sdl_tex);
        }
    }

    generation++;

    // ── Build entry ──────────────────────────────────────────────────────────
    GlyphEntry e;
    e.u0 = (float)ax           / ATLAS_W;
    e.v0 = (float)ay           / ATLAS_H;
    e.u1 = (float)(ax + dst_w) / ATLAS_W;
    e.v1 = (float)(ay + dst_h) / ATLAS_H;
    e.bw            = dst_w;
    e.bh            = dst_h;
    e.bearing_x     = slot->bitmap_left;
    e.bearing_y     = slot->bitmap_top;
    e.advance       = (int)((slot->advance.x >> 6) * glyph_scale);
    e.color         = is_color;
    e.render_scale  = glyph_scale;
    e.cell_offset_x = cell_offset_x;
    e.cell_offset_y = cell_offset_y;

    FT_Bitmap_Done(slot->library, &converted);
    cache[key] = e;
    return &cache[key];
}
