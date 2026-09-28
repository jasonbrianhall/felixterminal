#pragma once
// ============================================================================
// glyph_atlas.h — GPU texture atlas for rasterized glyphs.
//
// Each unique (codepoint, FT_Face*, font_px) triple is rasterized once into
// a 2048×2048 RGBA8 texture using a shelf packer.  Grayscale glyphs occupy
// one channel (stored as R8 logically, but packed into RGBA for simplicity);
// color BGRA emoji are stored as full RGBA.
//
// Invalidation
//   atlas_clear() — call on font-size change, face reload, or window resize
//   that changes font_px.  The next draw_text call repopulates on demand.
//
// Thread safety: single-threaded, same as the rest of the renderer.
// ============================================================================

#include <ft2build.h>
#include FT_FREETYPE_H
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>

// True only for real bitmap fonts (e.g. CBDT colour emoji): fixed strikes and
// no outlines. Scalable fonts that merely *contain* embedded strikes — MS
// Gothic, SimSun, Gulim and most Windows CJK fonts ship 1-bit strikes for
// small sizes — are rendered from their outlines instead.
static inline bool ft_face_uses_strikes(FT_Face f) {
    return f->num_fixed_sizes > 0 && !FT_IS_SCALABLE(f);
}

// ── Texture dimensions ────────────────────────────────────────────────────────
static constexpr int ATLAS_W       = 2048;
static constexpr int ATLAS_H       = 2048;
static constexpr int ATLAS_PADDING = 1;   // pixel gap between glyphs
// MAX_GLYPH_VERTS is defined in gl_renderer.h

// ── Per-glyph record ──────────────────────────────────────────────────────────
struct GlyphEntry {
    // UV coords into atlas texture (0..1)
    float u0, v0, u1, v1;
    // Pixel dimensions of the bitmap
    int bw, bh;
    // Bearing (bitmap_left / bitmap_top from FreeType)
    int bearing_x, bearing_y;
    // Horizontal advance in pixels (already >> 6)
    int advance;
    // True if the glyph is color (BGRA emoji) — shader handles differently
    bool color;
    // Scale factor applied when rasterizing (for bitmap/fixed-size faces)
    float render_scale;
    // For bitmap faces: offset to center glyph within the cell
    float cell_offset_x, cell_offset_y;
};

// ── Lookup key ────────────────────────────────────────────────────────────────
struct GlyphKey {
    uint32_t cp;
    FT_Face  face;   // pointer identity — fine since faces are long-lived
    int      font_px;

    bool operator==(const GlyphKey &o) const {
        return cp == o.cp && face == o.face && font_px == o.font_px;
    }
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey &k) const {
        size_t h = (size_t)k.cp;
        h ^= (size_t)(uintptr_t)k.face * 2654435761ULL + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= (size_t)k.font_px         * 2246822519ULL + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ── Atlas state ───────────────────────────────────────────────────────────────
struct GlyphAtlas {
    GLuint tex        = 0;
    SDL_Texture *sdl_tex = nullptr;  // SDL path atlas texture
    int    cursor_x  = ATLAS_PADDING;
    int    cursor_y  = ATLAS_PADDING;
    int    row_h     = 0;   // tallest glyph in the current shelf
    int    generation = 0;

    std::vector<uint8_t> pixels;  // CPU mirror of atlas — used by SDL path directly
    std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> cache;

    void init();
    void clear();   // invalidate — frees GPU texture, resets packer
    void destroy(); // full teardown at shutdown

    // Look up or rasterize a glyph.  Returns nullptr if the codepoint has no
    // glyph in the given face (caller should try a fallback face).
    const GlyphEntry *get(FT_Face face, uint32_t cp, int font_px, int emoji_px);
};

extern GlyphAtlas g_atlas;
