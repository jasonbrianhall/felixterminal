#include "ft_font.h"
#include "gl_terminal.h"   // ATTR_BOLD, ATTR_ITALIC
#include "glyph_atlas.h"   // GlyphAtlas g_atlas

// Embedded font headers
#include "DejaVuMonoBold.h"
#include "NotoEmoji.h"
#include "DejaVuMono.h"
#include "DejaVuMonoOblique.h"
#include "DejaVuMonoBoldOblique.h"
#include "FreeMono.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>

// ============================================================================
// GLOBALS
// ============================================================================

FT_Library s_ft_lib       = NULL;
FT_Face    s_ft_face      = NULL;
FT_Face    s_ft_face_reg  = NULL;
FT_Face    s_ft_face_obl  = NULL;
FT_Face    s_ft_face_bobl = NULL;
FT_Face    s_emoji_face   = NULL;
FT_Face    s_symbols_face = NULL;
FT_Face    s_cjk_face     = NULL;

unsigned char *s_font_buf      = NULL;
unsigned char *s_font_buf_reg  = NULL;
unsigned char *s_font_buf_obl  = NULL;
unsigned char *s_font_buf_bobl = NULL;
static unsigned char *s_emoji_buf = NULL;

extern int g_font_size;

static void load_cjk_fallback(void);  // defined below ft_init

// ============================================================================
// INIT
// ============================================================================

void ft_init(void) {
    FT_Init_FreeType(&s_ft_lib);

    size_t decoded_size = 0;
    int r = base64_decode(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE, &s_font_buf, &decoded_size);
    if (r != 0 || !s_font_buf) return;

    FT_New_Memory_Face(s_ft_lib, s_font_buf, (FT_Long)decoded_size, 0, &s_ft_face);
    SDL_Log("[Font] loaded: %s %s\n", s_ft_face->family_name, s_ft_face->style_name);

    auto load_variant = [&](const char *b64, size_t b64_size,
                            unsigned char **buf, FT_Face *face) {
        size_t sz = 0;
        if (base64_decode(b64, b64_size, buf, &sz) == 0 && *buf) {
            if (FT_New_Memory_Face(s_ft_lib, *buf, (FT_Long)sz, 0, face) == 0)
                SDL_Log("[Font] loaded: %s %s\n", (*face)->family_name, (*face)->style_name);
            else { free(*buf); *buf = nullptr; }
        }
    };
    load_variant(DEJAVU_REGULAR_FONT_B64,     DEJAVU_REGULAR_FONT_B64_SIZE,     &s_font_buf_reg,  &s_ft_face_reg);
    load_variant(DEJAVU_OBLIQUE_FONT_B64,     DEJAVU_OBLIQUE_FONT_B64_SIZE,     &s_font_buf_obl,  &s_ft_face_obl);
    load_variant(DEJAVU_BOLDOBLIQUE_FONT_B64, DEJAVU_BOLDOBLIQUE_FONT_B64_SIZE, &s_font_buf_bobl, &s_ft_face_bobl);

    size_t emoji_decoded_size = 0;
    int er = base64_decode(NOTOEMOJI_FONT_B64, NOTOEMOJI_FONT_B64_SIZE, &s_emoji_buf, &emoji_decoded_size);
    if (er == 0 && s_emoji_buf) {
        if (FT_New_Memory_Face(s_ft_lib, s_emoji_buf, (FT_Long)emoji_decoded_size, 0, &s_emoji_face) == 0)
            SDL_Log("[Font] emoji: %s %s\n", s_emoji_face->family_name, s_emoji_face->style_name);
        else { free(s_emoji_buf); s_emoji_buf = nullptr; }
    }

    {
        unsigned char *buf = nullptr;
        size_t sz = 0;
        if (base64_decode(FREEMONO_FONT_B64, FREEMONO_FONT_B64_SIZE, &buf, &sz) == 0 && buf) {
            if (FT_New_Memory_Face(s_ft_lib, buf, (FT_Long)sz, 0, &s_symbols_face) == 0) {
                SDL_Log("[Font] symbols: %s %s (embedded FreeMono)\n",
                        s_symbols_face->family_name, s_symbols_face->style_name);
                s_symbols_face->generic.data = buf;
            } else {
                free(buf);
            }
        }
    }

    load_cjk_fallback();
}

// ============================================================================
// CJK FALLBACK — none of the embedded fonts cover Chinese/Japanese/Korean,
// and embedding them would add 15+ MB, so borrow fonts from the system.
//
// One face per script, because no single stock font covers everything well:
// MS Gothic has no Hangul, Malgun Gothic has almost no kanji, and Chinese
// text drawn with a Japanese font uses the wrong letterforms.
//   - Kana            -> Japanese face
//   - Hangul          -> Korean face
//   - Bopomofo        -> Traditional Chinese face
//   - Han ideographs, CJK punctuation, full-width forms -> the face for the
//     user's locale (ja / ko / zh-Hans / zh-Hant), Japanese if not CJK
// If the preferred face lacks a character, any other loaded CJK face is used.
//
// Overrides:
//   FELIX_CJK_FONT=/path/font   tried first for every CJK character
//   FELIX_CJK_PREFER=ja|ko|zh-hans|zh-hant   which face draws Han characters
// ============================================================================

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // GetUserDefaultUILanguage
#endif

enum CjkScript { CJK_JA = 0, CJK_KO, CJK_ZH_HANS, CJK_ZH_HANT, CJK_COUNT };
static const char *const CJK_NAMES[CJK_COUNT] = { "Japanese", "Korean", "Chinese (Simplified)",
                                                  "Chinese (Traditional)" };
// A character each face must contain to count as covering its script
static const uint32_t CJK_PROBE[CJK_COUNT] = { 0x3042 /* あ */, 0xD55C /* 한 */,
                                               0x4E2D /* 中 */, 0x4E2D /* 中 */ };

struct CjkLoaded { std::string path; long index; FT_Face face; };
static std::vector<CjkLoaded> s_cjk_loaded;        // owns every CJK face (deduplicated)
static FT_Face  s_cjk_by_script[CJK_COUNT] = {};
static FT_Face  s_cjk_override = nullptr;          // FELIX_CJK_FONT
static CjkScript s_han_pref = CJK_JA;

// Open (or reuse) a face; returns nullptr if it can't be opened or lacks probe
static FT_Face open_cjk_face(const std::string &path, long index, uint32_t probe) {
    if (path.empty()) return nullptr;
    for (const CjkLoaded &l : s_cjk_loaded)
        if (l.path == path && l.index == index)
            return FT_Get_Char_Index(l.face, probe) ? l.face : nullptr;
    FT_Face f = nullptr;
    if (FT_New_Face(s_ft_lib, path.c_str(), index, &f) != 0) return nullptr;
    if (!FT_Get_Char_Index(f, probe)) { FT_Done_Face(f); return nullptr; }
    s_cjk_loaded.push_back({ path, index, f });
    return f;
}

static CjkScript cjk_pref_from_tag(const char *tag) {
    if (!tag || !*tag) return CJK_COUNT;
    std::string t(tag);
    for (char &c : t) c = (char)tolower((unsigned char)c);
    if (t.compare(0, 2, "ja") == 0) return CJK_JA;
    if (t.compare(0, 2, "ko") == 0) return CJK_KO;
    if (t.compare(0, 2, "zh") == 0) {
        if (t.find("hant") != std::string::npos || t.find("tw") != std::string::npos ||
            t.find("hk")   != std::string::npos || t.find("mo") != std::string::npos)
            return CJK_ZH_HANT;
        return CJK_ZH_HANS;
    }
    return CJK_COUNT;
}

static CjkScript detect_han_preference(void) {
    CjkScript p = cjk_pref_from_tag(getenv("FELIX_CJK_PREFER"));
    if (p != CJK_COUNT) return p;
#ifdef _WIN32
    LANGID lang = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(lang)) {
    case LANG_JAPANESE: return CJK_JA;
    case LANG_KOREAN:   return CJK_KO;
    case LANG_CHINESE:
        switch (SUBLANGID(lang)) {
        case SUBLANG_CHINESE_TRADITIONAL: case SUBLANG_CHINESE_HONGKONG:
        case SUBLANG_CHINESE_MACAU:       return CJK_ZH_HANT;
        default:                          return CJK_ZH_HANS;
        }
    }
#else
    for (const char *var : { "LC_ALL", "LC_CTYPE", "LANG" }) {
        const char *v = getenv(var);
        if (v && *v) { p = cjk_pref_from_tag(v); if (p != CJK_COUNT) return p; break; }
    }
#endif
    return CJK_JA;   // non-CJK locale: keep the previous default
}

#if !defined(_WIN32) && !defined(__APPLE__)
// Ask fontconfig without linking it: fc-match prints file and face index
static bool fc_match(const char *pattern, std::string &file, long &index) {
    std::string cmd = std::string("fc-match -f '%{file}\\n%{index}' '") + pattern + "' 2>/dev/null";
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) return false;
    char f[1024] = {0}, idx[32] = {0};
    if (fgets(f, sizeof(f), fp)) {
        f[strcspn(f, "\r\n")] = 0;
        if (!fgets(idx, sizeof(idx), fp)) idx[0] = 0;
    }
    pclose(fp);
    file = f; index = atol(idx);
    return !file.empty();
}
#endif

static void load_cjk_fallback(void) {
    s_han_pref = detect_han_preference();

    if (const char *ov = getenv("FELIX_CJK_FONT")) {
        // Accept any CJK-looking font here: kana, Hangul or Han
        for (uint32_t probe : { 0x4E2Du, 0x3042u, 0xD55Cu })
            if ((s_cjk_override = open_cjk_face(ov, 0, probe))) break;
        if (s_cjk_override)
            SDL_Log("[Font] CJK override: %s %s (%s)\n", s_cjk_override->family_name,
                    s_cjk_override->style_name, ov);
    }

    struct Cand { std::string path; long index; };
    std::vector<Cand> cands[CJK_COUNT];
#ifdef _WIN32
    const char *windir = getenv("WINDIR");
    const std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    auto W = [&](const char *n) { return fonts + n; };
    cands[CJK_JA]      = { { W("msgothic.ttc"), 0 }, { W("YuGothM.ttc"), 0 }, { W("meiryo.ttc"), 0 } };
    cands[CJK_KO]      = { { W("malgun.ttf"), 0 },   { W("gulim.ttc"), 0 } };
    cands[CJK_ZH_HANS] = { { W("msyh.ttc"), 0 },     { W("simsun.ttc"), 0 } };
    cands[CJK_ZH_HANT] = { { W("msjh.ttc"), 0 },     { W("mingliu.ttc"), 0 } };
#elif defined(__APPLE__)
    cands[CJK_JA]      = { { "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", 0 } };
    cands[CJK_KO]      = { { "/System/Library/Fonts/AppleSDGothicNeo.ttc", 0 } };
    cands[CJK_ZH_HANS] = { { "/System/Library/Fonts/Hiragino Sans GB.ttc", 0 },
                           { "/Library/Fonts/Arial Unicode.ttf", 0 } };
    cands[CJK_ZH_HANT] = { { "/Library/Fonts/Arial Unicode.ttf", 0 } };
#else
    // Noto CJK .ttc collections: index 0 = JP, 1 = KR, 2 = SC, 3 = TC
    static const char *const noto[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansMonoCJK-Regular.ttc" };
    static const char *const generic[] = {
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/google-droid/DroidSansFallbackFull.ttf" };
    for (int s = 0; s < CJK_COUNT; s++) {
        for (const char *n : noto)    cands[s].push_back({ n, (long)s });
        for (const char *g : generic) cands[s].push_back({ g, 0 });
    }
    static const char *const fc_lang[CJK_COUNT] = { "monospace:lang=ja", "monospace:lang=ko",
                                                   "monospace:lang=zh-cn", "monospace:lang=zh-tw" };
    static std::string fc_file[CJK_COUNT];
#endif

    for (int s = 0; s < CJK_COUNT; s++) {
        FT_Face f = nullptr;
#if !defined(_WIN32) && !defined(__APPLE__)
        long idx = 0;
        if (fc_match(fc_lang[s], fc_file[s], idx))
            f = open_cjk_face(fc_file[s], idx, CJK_PROBE[s]);
#endif
        for (const Cand &c : cands[s]) {
            if (f) break;
            f = open_cjk_face(c.path, c.index, CJK_PROBE[s]);
        }
        s_cjk_by_script[s] = f;
        if (f)
            SDL_Log("[Font] CJK %s: %s %s\n", CJK_NAMES[s], f->family_name, f->style_name);
    }

    // Primary face (kept for compatibility): the one that draws Han characters
    s_cjk_face = s_cjk_by_script[s_han_pref];
    for (int s = 0; !s_cjk_face && s < CJK_COUNT; s++) s_cjk_face = s_cjk_by_script[s];
    if (!s_cjk_face) s_cjk_face = s_cjk_override;

    if (s_cjk_face)
        SDL_Log("[Font] Han characters use the %s face\n", CJK_NAMES[s_han_pref]);
    else
        SDL_Log("[Font] no CJK font found — CJK text will show as boxes "
                "(install Noto Sans CJK, or set FELIX_CJK_FONT=/path/to/font)\n");
}

// Which script's face should draw this code point (CJK_COUNT = locale default)
static CjkScript cjk_script_of(uint32_t cp) {
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) ||
        (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7FF) ||
        (cp >= 0xFFA0 && cp <= 0xFFDC))
        return CJK_KO;                                   // Hangul
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) ||
        (cp >= 0xFF66 && cp <= 0xFF9F) || (cp >= 0x1B000 && cp <= 0x1B16F))
        return CJK_JA;                                   // Hiragana, Katakana
    if ((cp >= 0x3100 && cp <= 0x312F) || (cp >= 0x31A0 && cp <= 0x31BF))
        return CJK_ZH_HANT;                              // Bopomofo
    return CJK_COUNT;
}

// Best loaded CJK face that has this character, or nullptr
FT_Face ft_cjk_face_for(uint32_t cp) {
    if (s_cjk_override && FT_Get_Char_Index(s_cjk_override, cp)) return s_cjk_override;
    CjkScript sc = cjk_script_of(cp);
    CjkScript want = (sc == CJK_COUNT) ? s_han_pref : sc;
    FT_Face f = s_cjk_by_script[want];
    if (f && FT_Get_Char_Index(f, cp)) return f;
    for (int s = 0; s < CJK_COUNT; s++) {
        f = s_cjk_by_script[s];
        if (f && FT_Get_Char_Index(f, cp)) return f;
    }
    return nullptr;
}

void ft_shutdown(void) {
    g_atlas.destroy();

    if (s_symbols_face) {
        if (s_symbols_face->generic.data) free(s_symbols_face->generic.data);
        FT_Done_Face(s_symbols_face); s_symbols_face = nullptr;
    }
    for (CjkLoaded &l : s_cjk_loaded) FT_Done_Face(l.face);   // each face exactly once
    s_cjk_loaded.clear();
    for (FT_Face &f : s_cjk_by_script) f = nullptr;
    s_cjk_face = nullptr;
    s_cjk_override = nullptr;
    if (s_emoji_face)    FT_Done_Face(s_emoji_face);
    if (s_ft_face_bobl)  FT_Done_Face(s_ft_face_bobl);
    if (s_ft_face_obl)   FT_Done_Face(s_ft_face_obl);
    if (s_ft_face_reg)   FT_Done_Face(s_ft_face_reg);
    if (s_ft_face)       FT_Done_Face(s_ft_face);
    if (s_ft_lib)        FT_Done_FreeType(s_ft_lib);
    if (s_emoji_buf)     free(s_emoji_buf);
    if (s_font_buf_bobl) free(s_font_buf_bobl);
    if (s_font_buf_obl)  free(s_font_buf_obl);
    if (s_font_buf_reg)  free(s_font_buf_reg);
    if (s_font_buf)      free(s_font_buf);
}

FT_Face ft_make_menu_face(int pixel_size) {
    if (!s_ft_lib) return nullptr;
    unsigned char *buf = nullptr;
    size_t sz = 0;
    if (base64_decode(DEJAVU_REGULAR_FONT_B64, DEJAVU_REGULAR_FONT_B64_SIZE, &buf, &sz) != 0 || !buf)
        return nullptr;
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(s_ft_lib, buf, (FT_Long)sz, 0, &face) != 0) {
        free(buf); return nullptr;
    }
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_size);
    face->generic.data      = buf;
    face->generic.finalizer = [](void *obj) {
        FT_Face f = (FT_Face)obj;
        if (f->generic.data) { free(f->generic.data); f->generic.data = nullptr; }
    };
    return face;
}

void ft_reload_embedded(void) {
    // Invalidate the atlas — all cached glyphs are stale after a face reload.
    g_atlas.clear();

    auto free_face = [](FT_Face *face, unsigned char **buf) {
        if (*face) { FT_Done_Face(*face); *face = nullptr; }
        if (*buf)  { free(*buf); *buf = nullptr; }
    };
    free_face(&s_ft_face,      &s_font_buf);
    free_face(&s_ft_face_reg,  &s_font_buf_reg);
    free_face(&s_ft_face_obl,  &s_font_buf_obl);
    free_face(&s_ft_face_bobl, &s_font_buf_bobl);

    size_t decoded_size = 0;
    if (base64_decode(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE,
                      &s_font_buf, &decoded_size) == 0 && s_font_buf)
        FT_New_Memory_Face(s_ft_lib, s_font_buf, (FT_Long)decoded_size, 0, &s_ft_face);

    auto reload = [&](const char *b64, size_t b64_size,
                      unsigned char **buf, FT_Face *face) {
        size_t sz = 0;
        if (base64_decode(b64, b64_size, buf, &sz) == 0 && *buf)
            if (FT_New_Memory_Face(s_ft_lib, *buf, (FT_Long)sz, 0, face) != 0)
                { free(*buf); *buf = nullptr; }
    };
    reload(DEJAVU_REGULAR_FONT_B64,     DEJAVU_REGULAR_FONT_B64_SIZE,     &s_font_buf_reg,  &s_ft_face_reg);
    reload(DEJAVU_OBLIQUE_FONT_B64,     DEJAVU_OBLIQUE_FONT_B64_SIZE,     &s_font_buf_obl,  &s_ft_face_obl);
    reload(DEJAVU_BOLDOBLIQUE_FONT_B64, DEJAVU_BOLDOBLIQUE_FONT_B64_SIZE, &s_font_buf_bobl, &s_ft_face_bobl);
}

// ============================================================================
// FACE SELECTION
// ============================================================================

FT_Face face_for_attrs(uint8_t attrs) {
    bool bold = (attrs & ATTR_BOLD)   != 0;
    bool ital = (attrs & ATTR_ITALIC) != 0;
    if (bold && ital) return s_ft_face_bobl ? s_ft_face_bobl : s_ft_face;
    if (bold)         return s_ft_face;
    if (ital)         return s_ft_face_obl  ? s_ft_face_obl  : s_ft_face_reg;
    return s_ft_face_reg ? s_ft_face_reg : s_ft_face;
}

// ============================================================================
// UTF-8 UTILITIES
// ============================================================================

uint32_t next_codepoint(const unsigned char **p) {
    uint32_t cp;
    unsigned char c = **p;
    if (c < 0x80)       { cp = c;                      *p += 1; }
    else if (c < 0xE0)  { cp = (c&0x1F)<<6  | ((*p)[1]&0x3F);             *p += 2; }
    else if (c < 0xF0)  { cp = (c&0x0F)<<12 | ((*p)[1]&0x3F)<<6 | ((*p)[2]&0x3F); *p += 3; }
    else                { cp = (c&0x07)<<18 | ((*p)[1]&0x3F)<<12| ((*p)[2]&0x3F)<<6|((*p)[3]&0x3F); *p += 4; }
    return cp;
}

bool is_emoji_codepoint(uint32_t cp) {
    return (cp >= 0x2600  && cp <= 0x27BF)
        || (cp >= 0x1F300 && cp <= 0x1FAFF)
        || (cp >= 0x1F000 && cp <= 0x1F02F)
        || (cp >= 0xFE00  && cp <= 0xFE0F);
}

int cp_to_utf8(uint32_t cp, char *buf) {
    if (cp < 0x80)    { buf[0]=(char)cp; return 1; }
    if (cp < 0x800)   { buf[0]=(char)(0xC0|(cp>>6)); buf[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000) { buf[0]=(char)(0xE0|(cp>>12)); buf[1]=(char)(0x80|((cp>>6)&0x3F)); buf[2]=(char)(0x80|(cp&0x3F)); return 3; }
    buf[0]=(char)(0xF0|(cp>>18)); buf[1]=(char)(0x80|((cp>>12)&0x3F)); buf[2]=(char)(0x80|((cp>>6)&0x3F)); buf[3]=(char)(0x80|(cp&0x3F)); return 4;
}

// ============================================================================
// ATLAS-BACKED GLYPH RENDERING
// ============================================================================

// Push one quad (6 GlyphVertex) for an atlas-cached glyph.
// Returns the pixel advance so the caller can advance cx.
static float push_glyph_quad(const GlyphEntry *e,
                              float cx, 
                              float baseline_y,
                              float r, float g, float b, float a,
                              bool is_bitmap_face,
                              std::vector<GlyphVertex> &verts) {
    if (e->bw == 0 || e->bh == 0) return (float)e->advance;

    float gx, gy;
    if (is_bitmap_face) {
        // Bitmap/fixed-size face: center horizontally, align top of cell
        gx = cx + e->cell_offset_x;
        gy = baseline_y - (float)e->bh;
    } else {
        gx = cx + (float)e->bearing_x;
        gy = baseline_y - (float)e->bearing_y;
    }

    float x0 = gx,            y0 = gy;
    float x1 = gx + e->bw,    y1 = gy + e->bh;
    float u0 = e->u0, v0 = e->v0;
    float u1 = e->u1, v1 = e->v1;
    float cg = e->color ? 1.f : 0.f;

    // Two triangles, six vertices
    GlyphVertex q[6] = {
        {x0,y0, u0,v0, r,g,b,a, cg},
        {x1,y0, u1,v0, r,g,b,a, cg},
        {x1,y1, u1,v1, r,g,b,a, cg},
        {x0,y0, u0,v0, r,g,b,a, cg},
        {x1,y1, u1,v1, r,g,b,a, cg},
        {x0,y1, u0,v1, r,g,b,a, cg},
    };
    for (auto &v : q) verts.push_back(v);
    return (float)e->advance;
}

// ── draw_glyph: kept for API compatibility with any callers outside draw_text.
// Now redirects through the atlas instead of rasterizing per-frame.
float draw_glyph(FT_Face face, uint32_t cp, float cx, float baseline_y,
                 int font_px, int emoji_px, float r, float g, float b, float a,
                 std::vector<Vertex> & /*verts_unused*/) {
    // We ignore the legacy Vertex vector — glyph data now goes to the glyph
    // accumulator via draw_glyph_verts.  The old verts param is kept so
    // existing call sites compile without changes.
    const GlyphEntry *e = g_atlas.get(face, cp, font_px, emoji_px);
    if (!e) return 0.f;

    bool is_bitmap = ft_face_uses_strikes(face);
    float tr = r, tg = g, tb = b;
    // Grayscale emoji (non-color NotoEmoji entries) render white so the tint
    // colours them; colour BGRA entries ignore tint rgb anyway.
    if (face == s_emoji_face && !e->color) { tr = 1.f; tg = 1.f; tb = 1.f; }

    GlyphVertex quad[6];
    // Build quad inline
    float gx, gy;
    if (is_bitmap) { gx = cx + e->cell_offset_x; gy = baseline_y - (float)e->bh; }
    else           { gx = cx + (float)e->bearing_x; gy = baseline_y - (float)e->bearing_y; }

    if (e->bw > 0 && e->bh > 0) {
        float x0=gx, y0=gy, x1=gx+e->bw, y1=gy+e->bh;
        float cg = e->color ? 1.f : 0.f;
        quad[0]={x0,y0,e->u0,e->v0,tr,tg,tb,a,cg};
        quad[1]={x1,y0,e->u1,e->v0,tr,tg,tb,a,cg};
        quad[2]={x1,y1,e->u1,e->v1,tr,tg,tb,a,cg};
        quad[3]={x0,y0,e->u0,e->v0,tr,tg,tb,a,cg};
        quad[4]={x1,y1,e->u1,e->v1,tr,tg,tb,a,cg};
        quad[5]={x0,y1,e->u0,e->v1,tr,tg,tb,a,cg};
        draw_glyph_verts(quad, 6);
    }
    return (float)e->advance;
}

// ── draw_text: atlas-backed, one textured quad per glyph ─────────────────────
float draw_text(const char *text, float x, float y, int font_px, int emoji_px,
                float r, float g, float b, float a, uint8_t attrs, float box_w) {
    if (!s_ft_face || !text || !*text) return x;

    // Thread-local reused vector to avoid heap churn per call
    static std::vector<GlyphVertex> verts;
    verts.clear();

    FT_Face base_face = face_for_attrs(attrs);

    float cx = x;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        if (!cp || cp == 0xFE0F) continue;

        // Face selection — mirror original priority order exactly
        FT_Face face = base_face;
        if (s_symbols_face && cp >= 0x2600 && cp <= 0x27BF) {
            if (FT_Get_Char_Index(s_symbols_face, cp)) face = s_symbols_face;
        }
        if (face == base_face && s_emoji_face && is_emoji_codepoint(cp)) {
            if (FT_Get_Char_Index(s_emoji_face, cp)) face = s_emoji_face;
        }

        float tr = r, tg = g, tb = b;
        bool is_bitmap = ft_face_uses_strikes(face);
        bool is_gray_emoji = (face == s_emoji_face && !is_bitmap);
        if (is_gray_emoji) { tr = 1.f; tg = 1.f; tb = 1.f; }

        // Try primary face
        const GlyphEntry *e = g_atlas.get(face, cp, font_px, emoji_px);

        // CJK comes before the emoji/symbol fallbacks — the embedded fonts
        // have no CJK at all, so this is the only face that can supply it.
        if (!e && !is_emoji_codepoint(cp)) {
            FT_Face cjk = ft_cjk_face_for(cp);
            if (cjk && cjk != face) e = g_atlas.get(cjk, cp, font_px, font_px);
        }

        // Fallback chain — same order as original draw_text
        if (!e && face != s_emoji_face && s_emoji_face)
            e = g_atlas.get(s_emoji_face, cp, font_px, emoji_px);
        if (!e && face != s_symbols_face && s_symbols_face)
            e = g_atlas.get(s_symbols_face, cp, font_px, font_px);

        float adv = 0.f;
        if (e) {
            // Determine tint for whichever face actually provided the glyph
            // (may differ from initial face after fallback)
            float er = tr, eg = tg, eb = tb;
            if (!e->color) {
                // Keep caller tint for normal glyphs; emoji fallbacks go white
                // (already set above for is_gray_emoji; reset here if we fell
                // back to emoji face from a non-emoji primary face)
            }

            adv = push_glyph_quad(e, cx, y, er, eg, eb, a,
                                  is_bitmap, verts);
        }

        // Final fallback: draw a replacement rectangle for unknown glyphs
        if (adv == 0.f) {
            FT_Set_Pixel_Sizes(base_face, 0, (FT_UInt)font_px);
            FT_UInt gi = FT_Get_Char_Index(base_face, ' ');
            if (!FT_Load_Glyph(base_face, gi, FT_LOAD_DEFAULT))
                adv = (float)(base_face->glyph->advance.x >> 6);
            if (adv == 0.f) adv = (float)font_px * 0.6f;

            float t  = 1.f;
            if (box_w > 0.f) adv = box_w;   // double-width cell: box spans both
            float bh = (float)font_px * 0.7f;
            float bw = adv - 2.f;
            float bx = cx + 1.f;
            float by = y - bh;

            // Flush pending glyph verts before switching to rect drawing
            if (!verts.empty()) {
                draw_glyph_verts(verts.data(), (int)verts.size());
                verts.clear();
            }
            // Top, bottom, left, right border strips
            draw_rect(bx,      by,      bw, t,  r, g, b, a);
            draw_rect(bx,      by+bh-t, bw, t,  r, g, b, a);
            draw_rect(bx,      by,      t,  bh, r, g, b, a);
            draw_rect(bx+bw-t, by,      t,  bh, r, g, b, a);
        }
        cx += adv;
    }

    if (!verts.empty())
        draw_glyph_verts(verts.data(), (int)verts.size());

    return cx;
}

// ── measure_text: unchanged (no rasterization, just advance queries) ──────────
float measure_text(const char *text, int font_px) {
    if (!s_ft_face || !text) return 0;
    FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)font_px);
    float w = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, cp);
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT))
            w += (float)(s_ft_face->glyph->advance.x >> 6);
    }
    return w;
}

// ── atlas_invalidate: call whenever font_px changes (font size slider, etc.) ──
void ft_invalidate_glyph_cache(void) {
    g_atlas.clear();
}
