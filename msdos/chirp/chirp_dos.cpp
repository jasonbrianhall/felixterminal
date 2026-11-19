// chirp_dos.cpp: Felix Chirp for MS-DOS (CHIRP.EXE), the front end.
//
// Felix Chirp is Felix Terminal's F5 viewer (felixchirp/): images, music,
// CD+G and .kfn karaoke, text and Markdown, and zip files, with a file
// browser. Here it is a program of its own: this file opens a VESA mode
// through SDL3's DOS port and gives felixchirp/ what it takes from the rest
// of felixterminal, which isn't in this build:
//
//   draw_rect, draw_text   (gl_renderer.cpp / ft_font.cpp): SDL's software
//                          renderer and FreeType with the embedded DejaVu
//                          Sans Mono (and Noto Emoji for what DejaVu lacks)
//   glBegin ... glEnd      the immediate-mode panel of the text viewer
//   ssh_get_session        always NULL: no remote (SFTP) browsing on DOS
//   mkstemps, mkstemp      temp files in %TEMP% (else C:\TEMP), 8.3 names
//
// felixchirp/ itself is compiled unchanged but for HAVE_GSTREAMER being off
// on __DJGPP__ (no video). Sound is mixer_dos.cpp (SDL_mixer's API on SDL3).

#include "felixchirp/felixchirp.h"
#include "gl_renderer.h"
#include "ft_font.h"
#include "ssh_session.h"

#include "DejaVuMono.h"
#include "DejaVuMonoBold.h"
#include "NotoEmoji.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ============================================================================
// WHAT FELIXCHIRP/ EXPECTS FROM FELIXTERMINAL
// ============================================================================

int           g_font_size        = 16;
bool          g_use_sdl_renderer = true;
SDL_Renderer *g_sdl_renderer     = nullptr;
SDL_Window   *g_sdl_window       = nullptr;
FT_Face       s_ft_face          = nullptr;

void gl_flush_verts(void) {}

LIBSSH2_SESSION *ssh_get_session() { return nullptr; }
void ssh_session_lock() {}
void ssh_session_unlock() {}

// ---- for CHIRP_LOG: where a frame's drawing time goes ----
#include <time.h>
static uclock_t s_t_rect, s_t_text, s_t_geo; static long s_n_rect, s_n_text, s_n_glyph;
// ---- draw_rect --------------------------------------------------------------

void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    if (w <= 0 || h <= 0 || a <= 0) return;
    uclock_t t0 = uclock(); s_n_rect++;
    // Whole pixels, at least one each way: the visualizer's 128 bars are
    // under a pixel wide at 640x480, and SDL fills those with nothing.
    float x0 = SDL_floorf(x + 0.5f), y0 = SDL_floorf(y + 0.5f);
    float x1 = SDL_floorf(x + w + 0.5f), y1 = SDL_floorf(y + h + 0.5f);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    struct P { uclock_t t0; ~P() { s_t_rect += uclock() - t0; } } prof{t0};
    SDL_SetRenderDrawBlendMode(g_sdl_renderer, a >= 1.f ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(g_sdl_renderer, r, g, b, a > 1.f ? 1.f : a);
    SDL_FRect rc = { x, y, w, h };
    SDL_RenderFillRect(g_sdl_renderer, &rc);
}

// ---- the text viewer's glBegin/glVertex2f/glEnd -----------------------------

static SDL_FColor s_gl_color = { 1, 1, 1, 1 };
static GLenum s_gl_mode;
static std::vector<SDL_FPoint> s_gl_pts;

void glColor4f(float r, float g, float b, float a) { s_gl_color = { r, g, b, a }; }
void glBegin(GLenum mode) { s_gl_mode = mode; s_gl_pts.clear(); }
void glVertex2f(float x, float y) { s_gl_pts.push_back({ x, y }); }
void glEnd(void) {
    SDL_SetRenderDrawBlendMode(g_sdl_renderer, SDL_BLENDMODE_BLEND);
    if (s_gl_mode == GL_QUADS) {
        for (size_t i = 0; i + 3 < s_gl_pts.size(); i += 4) {
            SDL_Vertex v[4];          // (SDL2's, with an 8-bit colour: include/SDL2/SDL.h)
            SDL_Color c = { (Uint8)(s_gl_color.r * 255), (Uint8)(s_gl_color.g * 255),
                            (Uint8)(s_gl_color.b * 255), (Uint8)(s_gl_color.a * 255) };
            for (int k = 0; k < 4; k++) v[k] = { s_gl_pts[i + k], c, { 0, 0 } };
            static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
            SDL_RenderGeometry(g_sdl_renderer, nullptr, v, 4, idx, 6);
        }
    } else if (s_gl_mode == GL_LINE_LOOP && s_gl_pts.size() > 1) {
        SDL_SetRenderDrawColorFloat(g_sdl_renderer, s_gl_color.r, s_gl_color.g, s_gl_color.b, s_gl_color.a);
        s_gl_pts.push_back(s_gl_pts[0]);
        SDL_RenderLines(g_sdl_renderer, s_gl_pts.data(), (int)s_gl_pts.size());
    }
    s_gl_pts.clear();
}

// ---- text ---------------------------------------------------------------------

FT_Library        s_ft_lib;       // (ft_font.h has it extern)
static FT_Face    s_face[2];       // DejaVu Sans Mono, regular and bold
static FT_Face    s_emoji;         // Noto Emoji

static FT_Face load_face(const char *b64, size_t b64_size) {
    unsigned char *buf = nullptr;
    size_t sz = 0;
    FT_Face f = nullptr;
    if (base64_decode(b64, b64_size, &buf, &sz) == 0 && buf &&
        FT_New_Memory_Face(s_ft_lib, buf, (FT_Long)sz, 0, &f) == 0)
        return f;
    free(buf);
    return nullptr;
}

static bool font_init(void) {
    if (FT_Init_FreeType(&s_ft_lib)) return false;
    s_face[0] = load_face(DEJAVU_REGULAR_FONT_B64, DEJAVU_REGULAR_FONT_B64_SIZE);
    s_face[1] = load_face(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE);
    s_emoji   = load_face(NOTOEMOJI_FONT_B64, NOTOEMOJI_FONT_B64_SIZE);
    if (!s_face[0]) return false;
    if (!s_face[1]) s_face[1] = s_face[0];
    s_ft_face = s_face[0];
    return true;
}

// Glyphs: FreeType's coverage, kept as bytes. Text is drawn a string at a
// time: each (text, size, bold) made into one white texture with the
// coverage in alpha, kept and coloured as it's drawn. Nearly all of the
// screen's text is the same from frame to frame, and one textured draw
// a string, not one a glyph, is what makes SDL's software renderer fast
// enough for the visualizer.
struct Glyph {
    std::vector<uint8_t> cov;      // w x h coverage (empty: blank)
    int left = 0, top = 0, w = 0, h = 0;
    float adv = 0;
};
static std::unordered_map<uint64_t, Glyph> s_glyphs;

static uint32_t next_cp(const unsigned char **pp) {
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= (0x3F >> extra);
    while (extra-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *pp = p;
    return c;
}

static const Glyph &glyph(uint32_t cp, int px, int bold) {
    uint64_t key = (uint64_t)cp | (uint64_t)px << 32 | (uint64_t)bold << 48;
    auto it = s_glyphs.find(key);
    if (it != s_glyphs.end()) return it->second;
    Glyph g;
    FT_Face f = s_face[bold];
    if (!FT_Get_Char_Index(f, cp) && s_emoji && FT_Get_Char_Index(s_emoji, cp)) f = s_emoji;
    FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px);
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0) {
        FT_GlyphSlot sl = f->glyph;
        g.left = sl->bitmap_left; g.top = sl->bitmap_top;
        g.w = (int)sl->bitmap.width; g.h = (int)sl->bitmap.rows;
        g.adv = sl->advance.x / 64.f;
        if (f == s_emoji) {                      // emoji are wider than a cell: one and a half
            float cell = px * 0.6f * 1.5f;
            if (g.adv > cell) g.adv = cell;
        }
        if (g.w > 0 && g.h > 0) {
            g.cov.resize((size_t)g.w * g.h);
            for (int y = 0; y < g.h; y++)
                memcpy(&g.cov[(size_t)y * g.w], sl->bitmap.buffer + y * sl->bitmap.pitch, (size_t)g.w);
        }
    } else {
        g.adv = px * 0.6f;
    }
    return s_glyphs.emplace(key, g).first->second;
}

struct TextImage {
    SDL_Texture *tex = nullptr;    // nullptr: nothing to draw (spaces)
    int x0 = 0, top = 0, w = 0, h = 0;   // the texture's place from the pen and the baseline
    float adv = 0;
    unsigned long used = 0;        // the frame it was last drawn in
    bool drawable = false;         // made to be drawn (false: only measured, no texture)
};
static std::unordered_map<std::string, TextImage> s_texts;
static unsigned long s_frame;      // (counted by main)
static size_t s_text_pixels;       // in all of them
// The most the kept texts may take, in pixels (4 bytes each): an eighth of
// the memory free at the start (main sets it), at least 192K pixels (two
// screens of text at 640x480), at most 1M (4 MB). They're also let go when
// a file is opened (forget_unseen_texts). DOSBox gives DOS 16 MB by default,
// about 6 of them free once Chirp's screen is open; scrolling through a few
// source files kept a texture per line seen, and the DPMI host then paged to
// disk: Chirp seemed to freeze.
static size_t s_text_budget = 1u << 20;
static int s_win_w, s_win_h;       // (the screen: no text texture is wider)

static void forget_text(std::unordered_map<std::string, TextImage>::iterator it) {
    if (it->second.tex) SDL_DestroyTexture(it->second.tex);
    s_text_pixels -= (size_t)it->second.w * it->second.h;
    s_texts.erase(it);
}

// Room for `need` more pixels: the texts drawn longest ago go first (none
// drawn this frame).
static void make_room_for_text(size_t need) {
    while (s_text_pixels + need > s_text_budget) {
        auto oldest = s_texts.end();
        for (auto it = s_texts.begin(); it != s_texts.end(); ++it)
            if (it->second.used != s_frame && (oldest == s_texts.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == s_texts.end()) break;          // (all on screen now: over the budget for a frame)
        forget_text(oldest);
    }
}

// All but what the last frame drew: a file is being opened, and the text of
// the one before (a page of a source file, say) won't be drawn again.
static void forget_unseen_texts(void) {
    for (auto it = s_texts.begin(); it != s_texts.end();) {
        auto next = std::next(it);
        if (it->second.used + 1 < s_frame) forget_text(it);
        it = next;
    }
}

static void forget_old_texts(void) {
    if (s_texts.size() > 2000) {                      // (many blank ones: drop those unused a while)
        for (auto it = s_texts.begin(); it != s_texts.end();) {
            auto next = std::next(it);
            if (s_frame - it->second.used > 120) forget_text(it);
            it = next;
        }
    }
}

static const TextImage &text_image(const char *text, int px, int bold, bool make_texture = true) {
    std::string key(text);
    key += '\0'; key += (char)px; key += (char)bold;
    auto it = s_texts.find(key);
    if (it != s_texts.end() && (it->second.drawable || !make_texture)) return it->second;
    if (it != s_texts.end()) s_texts.erase(it);       // (measured before; now it's drawn)

    // Where the glyphs go: their box, from the pen at 0 on the baseline.
    // Glyphs past the screen's width aren't kept (nor drawn): only the
    // advance counts them.
    const int max_w = s_win_w > 0 ? s_win_w : 4096;
    struct Placed { const Glyph *g; int x; };
    std::vector<Placed> pl;
    float cx = 0;
    int x0 = 0, x1 = 0, top = 0, bot = 0;
    bool any = false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_cp(&p);
        if (cp == 0xFE0F || cp == '\n' || cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        const Glyph &g = glyph(cp, px, bold);
        if (!g.cov.empty() && make_texture && (int)(cx + g.left) + g.w <= max_w) {
            int gx = (int)(cx + g.left + 0.5f);
            if (!any) { x0 = gx; x1 = gx + g.w; top = g.top; bot = g.h - g.top; any = true; }
            x0 = std::min(x0, gx); x1 = std::max(x1, gx + g.w);
            top = std::max(top, g.top); bot = std::max(bot, g.h - g.top);
            pl.push_back({ &g, gx });
        }
        cx += g.adv;
    }
    TextImage ti;
    ti.adv = cx;
    ti.drawable = make_texture;
    if (any && x1 > x0 && top + bot > 0) {
        ti.x0 = x0; ti.top = top; ti.w = x1 - x0; ti.h = top + bot;
        make_room_for_text((size_t)ti.w * ti.h);
        std::vector<uint32_t> px32((size_t)ti.w * ti.h, 0x00FFFFFF);
        for (const Placed &q : pl) {
            const Glyph &g = *q.g;
            for (int y = 0; y < g.h; y++) {
                uint32_t *row = &px32[(size_t)(top - g.top + y) * ti.w + (q.x - x0)];
                const uint8_t *c = &g.cov[(size_t)y * g.w];
                for (int x = 0; x < g.w; x++) {
                    uint32_t a = row[x] >> 24;
                    if (c[x] > a) row[x] = (uint32_t)c[x] << 24 | 0xFFFFFF;
                }
            }
        }
        ti.tex = SDL_CreateTexture(g_sdl_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, ti.w, ti.h);
        if (ti.tex) {
            SDL_UpdateTexture(ti.tex, nullptr, px32.data(), ti.w * 4);
            SDL_SetTextureBlendMode(ti.tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(ti.tex, SDL_SCALEMODE_NEAREST);
            s_text_pixels += (size_t)ti.w * ti.h;
        }
    }
    return s_texts.emplace(std::move(key), ti).first->second;
}

// y is the baseline. Returns the x after the text.
float draw_text(const char *text, float x, float y, int font_px, int, float r, float g, float b, float a,
                uint8_t attrs, float) {
    if (!text || !*text || font_px <= 0) return x;
    struct P { uclock_t t0; ~P() { s_t_text += uclock() - t0; } } prof{uclock()}; s_n_text++;
    TextImage &ti = const_cast<TextImage &>(text_image(text, font_px, (attrs & 1) ? 1 : 0));   // ATTR_BOLD
    ti.used = s_frame;
    if (ti.tex) {
        s_n_glyph++;
        SDL_SetTextureColorModFloat(ti.tex, r, g, b);
        SDL_SetTextureAlphaModFloat(ti.tex, a > 1.f ? 1.f : a);
        SDL_FRect d = { (float)(int)(x + 0.5f) + ti.x0, (float)(int)(y + 0.5f) - ti.top, (float)ti.w, (float)ti.h };
        SDL_RenderTexture(g_sdl_renderer, ti.tex, nullptr, &d);
    }
    return x + ti.adv;
}

float measure_text(const char *text, int font_px) {
    if (!text || !*text) return 0;
    return text_image(text, font_px, 0, false).adv;   // (no texture: just how wide)
}

// ---- temp files (DJGPP has no mkstemps) -------------------------------------

static std::string s_tmpdir;

static bool is_dir(const std::string &d) {
    struct stat st;
    return stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static const std::string &tmpdir() {
    if (!s_tmpdir.empty()) return s_tmpdir;
    const char *envs[] = { "TEMP", "TMP", "TMPDIR" };
    for (const char *e : envs) {
        const char *v = getenv(e);
        if (v && *v && is_dir(v)) { s_tmpdir = v; break; }
    }
    if (s_tmpdir.empty()) {
        s_tmpdir = "C:/TEMP";
        if (!is_dir(s_tmpdir)) mkdir(s_tmpdir.c_str(), 0755);
        if (!is_dir(s_tmpdir)) s_tmpdir = ".";
    }
    while (s_tmpdir.size() > 3 && (s_tmpdir.back() == '/' || s_tmpdir.back() == '\\')) s_tmpdir.pop_back();
    return s_tmpdir;
}

static int chirp_mkstemps(char *tmpl, int suffixlen, size_t room);

// felixchirp/ asks for "/tmp/iv_tmp_XXXXXX.ext" (in a 512-byte buffer); on
// DOS that becomes %TEMP%\CHxxxxxx.ext, a name that fits 8.3 without LFN.
extern "C" int mkstemps(char *tmpl, int suffixlen) {
    return chirp_mkstemps(tmpl, suffixlen, 512);
}
// midi_render.cpp's mkstemp("/tmp/midi_render_XXXXXX") (a 24-byte buffer):
// this one, linked in place of DJGPP's, keeps to the template's length,
// in the current folder if %TEMP%'s name is too long for it.
extern "C" int mkstemp(char *tmpl) {
    return chirp_mkstemps(tmpl, 0, strlen(tmpl) + 1);
}
static int chirp_mkstemps(char *tmpl, int suffixlen, size_t room) {
    std::string suffix = suffixlen > 0 ? std::string(tmpl + strlen(tmpl) - suffixlen) : "";
    std::string dir = tmpdir();
    const char *sep = (dir.back() == '/' || dir.back() == '\\') ? "" : "\\";
    static unsigned n = 0;
    if (!n) n = (unsigned)SDL_GetTicks() * 2654435761u;
    for (int tries = 0; tries < 1000; tries++) {
        char name[600];
        snprintf(name, sizeof name, "%s%sCH%06X%s", dir.c_str(), sep, (n++ & 0xFFFFFF), suffix.c_str());
        if (strlen(name) >= room) snprintf(name, sizeof name, "CH%06X%s", (n++ & 0xFFFFFF), suffix.c_str());
        if (strlen(name) >= room) { errno = ENAMETOOLONG; return -1; }
        int fd = open(name, O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
        if (fd >= 0) { strcpy(tmpl, name); return fd; }
        if (errno != EEXIST) return -1;
    }
    return -1;
}

extern FILE *g_chirp_log;        // mixer_dos.cpp
double chirp_music_position(void);
double chirp_audio_latency(void);

// ---- DOS critical errors ------------------------------------------------------
// A drive with no disk in it (A:), or one that can't be read, makes DOS ask
// "Abort, Retry, Fail?" through INT 24h: on the text screen, unseen in a
// VESA mode, and Abort ends Chirp there and then. This handler answers Fail
// at once, so the call just fails (an empty folder, "No drive A:"). DOS puts
// the old vector back when Chirp exits.
// For CHIRP_LOG: how much DPMI memory is free, in KB
static void log_memory(const char *when) {
    if (!g_chirp_log) return;
    __dpmi_free_mem_info fi;
    if (__dpmi_get_free_memory_information(&fi) == 0)
        fprintf(g_chirp_log, "memory %s: %lu KB free (largest block %lu KB)\n", when,
                fi.total_number_of_free_pages * 4, fi.largest_available_free_block_in_bytes / 1024);
    fflush(g_chirp_log);
}

static void fail_critical_errors(void) {
    static const unsigned char handler[] = { 0xB0, 0x03, 0xCF };   // mov al,3 (Fail); iret
    int sel = 0;
    int seg = __dpmi_allocate_dos_memory(1, &sel);                  // (16 bytes)
    if (seg == -1) return;
    dosmemput(handler, sizeof handler, seg * 16);
    __dpmi_raddr a = { 0, (unsigned short)seg };
    __dpmi_set_real_mode_interrupt_vector(0x24, &a);
}

// ---- "Loading ..." ----------------------------------------------------------
// felixchirp/ calls this (g_iv_busy_hook) before it opens a file; on DOS nothing else runs
// while it does (a .kfn's tracks take a few seconds), so the screen as it was
// gets a notice over it, shown at once.
static void show_busy(const char *name) {
    if (!g_sdl_renderer || !s_win_w) return;
    forget_unseen_texts();
    if (g_chirp_log) {
        __dpmi_free_mem_info fi;
        unsigned long freekb = __dpmi_get_free_memory_information(&fi) == 0 ? fi.total_number_of_free_pages * 4 : 0;
        fprintf(g_chirp_log, "%llu ms: opening %s (%lu KB free; %u texts, %u KB of them, budget %u KB; %u glyphs)\n",
                (unsigned long long)SDL_GetTicks(), name, freekb, (unsigned)s_texts.size(), (unsigned)(s_text_pixels * 4 / 1024),
                (unsigned)(s_text_budget * 4 / 1024), (unsigned)s_glyphs.size());
        fflush(g_chirp_log);
    }
    iv_render(s_win_w, s_win_h);
    char msg[300];
    snprintf(msg, sizeof msg, "Loading %s ...", name);
    int px = g_font_size;
    float tw = measure_text(msg, px);
    if (tw > s_win_w - 40) { snprintf(msg, sizeof msg, "Loading ..."); tw = measure_text(msg, px); }
    float bw = tw + 2.f * px, bh = 2.4f * px;
    float bx = (s_win_w - bw) * 0.5f, by = (s_win_h - bh) * 0.5f;
    draw_rect(bx, by, bw, bh, 0.10f, 0.12f, 0.20f, 0.95f);
    draw_rect(bx, by, bw, 1, 0.35f, 0.50f, 0.85f, 1.f);
    draw_rect(bx, by + bh - 1, bw, 1, 0.35f, 0.50f, 0.85f, 1.f);
    draw_rect(bx, by, 1, bh, 0.35f, 0.50f, 0.85f, 1.f);
    draw_rect(bx + bw - 1, by, 1, bh, 0.35f, 0.50f, 0.85f, 1.f);
    draw_text(msg, bx + px, by + bh * 0.5f + px * 0.35f, px, px, 0.90f, 0.95f, 1.f, 1.f, 0, 0);
    SDL_RenderPresent(g_sdl_renderer);
}

// ============================================================================
// THE SCREEN
// ============================================================================

static bool pick_mode(int want_w, int want_h, int want_bpp, SDL_DisplayMode *out) {
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
    if (!modes) return false;
    const SDL_DisplayMode *best = nullptr;
    long best_score = -1;
    for (int i = 0; i < n; i++) {
        const SDL_DisplayMode *m = modes[i];
        int bpp = SDL_BITSPERPIXEL(m->format);
        if (SDL_ISPIXELFORMAT_INDEXED(m->format) || bpp < 15 || m->w < 640 || m->h < 400) continue;
        if (want_bpp && bpp != want_bpp && SDL_BYTESPERPIXEL(m->format) * 8 != want_bpp) continue;
        long score;
        if (want_w) score = (m->w == want_w && m->h == want_h) ? 1000000 : 0;
        else score = (m->w == 640 && m->h == 480) ? 2000000 : 1000000 - (long)m->w * m->h / 64;   // 640x480, else the smallest
        int bytes = SDL_BYTESPERPIXEL(m->format);
        score = score * 4 + (bytes == 4 ? 3 : bpp == 16 ? 2 : bpp == 15 ? 1 : 0);
        if (score > best_score) { best_score = score; best = m; }
    }
    if (best) *out = *best;
    SDL_free(modes);
    return best != nullptr;
}

// The modes F2 / Shift+F2 step through: each size once (its deepest
// colours), 640x480 and up, smallest first.
static std::vector<SDL_DisplayMode> usable_modes(void) {
    std::vector<SDL_DisplayMode> out;
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
    for (int i = 0; modes && i < n; i++) {
        const SDL_DisplayMode *m = modes[i];
        if (SDL_ISPIXELFORMAT_INDEXED(m->format) || SDL_BITSPERPIXEL(m->format) < 15 || m->w < 640 || m->h < 400) continue;
        bool merged = false;
        for (SDL_DisplayMode &o : out)
            if (o.w == m->w && o.h == m->h) {
                if (SDL_BITSPERPIXEL(m->format) > SDL_BITSPERPIXEL(o.format)) o = *m;
                merged = true;
            }
        if (!merged) out.push_back(*m);
    }
    SDL_free(modes);
    std::sort(out.begin(), out.end(), [](const SDL_DisplayMode &a, const SDL_DisplayMode &b) {
        return a.w * a.h != b.w * b.h ? a.w * a.h < b.w * b.h : a.w < b.w;
    });
    return out;
}

static bool set_mode(const SDL_DisplayMode &m, int *w, int *h) {
    if (!SDL_SetWindowFullscreenMode(g_sdl_window, &m)) return false;
    SDL_SyncWindow(g_sdl_window);
    int ow = 0, oh = 0;
    if (!SDL_GetCurrentRenderOutputSize(g_sdl_renderer, &ow, &oh) || ow <= 0) { ow = m.w; oh = m.h; }
    *w = ow; *h = oh;
    return true;
}

static void list_modes(void) {
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
    printf("VESA modes:\n");
    for (int i = 0; modes && i < n; i++) {
        const SDL_DisplayMode *m = modes[i];
        if (SDL_ISPIXELFORMAT_INDEXED(m->format)) continue;
        printf("  %dx%dx%d\n", m->w, m->h, SDL_BYTESPERPIXEL(m->format) == 4 ? 32 : SDL_BITSPERPIXEL(m->format));
    }
    SDL_free(modes);
}

// ============================================================================
// MAIN
// ============================================================================

static void usage(void) {
    printf("Felix Chirp for DOS: pictures, music, karaoke and text.\n\n"
           "  CHIRP [-g WxH[xBITS]] [-f size] [-l] [folder | file]\n\n"
           "  folder   where to start (default: the current one)\n"
           "  file     open it at once (a picture, song, .cdg, .kfn, .zip, text)\n"
           "  -g       the VESA mode (default 640x480; also SET CHIRP_VIDEO=;\n"
           "           F2 / Shift+F2 change it while running)\n"
           "  -f       font size in pixels (default 16)\n"
           "  -l       list the VESA modes\n\n"
           "Sound: a Sound Blaster, with BLASTER set (SET BLASTER=A220 I5 D1 T4).\n"
           "Keys: arrows, Enter, Space (pause), S (stop), V (visualizer), R (rotate),\n"
           "+/- (volume), wheel (zoom), 0 (reset zoom), Esc (back, then quit), F10 (quit).\n"
           "Drives: Alt+letter goes to that drive; a drive's root lists the others.\n");
}

static bool parse_mode(const char *s, int *w, int *h, int *bpp) {
    int a = 0, b = 0, c = 0;
    int n = sscanf(s, "%dx%dx%d", &a, &b, &c);
    if (n < 2 || a < 320 || b < 200) return false;
    *w = a; *h = b; *bpp = n == 3 ? c : 0;
    return true;
}

// "c:/music" -> "C:\MUSIC"-style with backslashes, no trailing one but at the root.
static std::string dos_path(std::string p) {
    for (char &c : p) if (c == '/') c = '\\';
    if (p.size() >= 2 && p[1] == ':') p[0] = (char)toupper((unsigned char)p[0]);
    while (p.size() > 3 && p.back() == '\\') p.pop_back();
    if (p.size() == 2 && p[1] == ':') p += '\\';
    return p;
}


int main(int argc, char **argv) {
    if (getenv("CHIRP_LOG")) g_chirp_log = fopen(getenv("CHIRP_LOG"), "w");
    fail_critical_errors();
    {
        __dpmi_free_mem_info fi;
        if (__dpmi_get_free_memory_information(&fi) == 0 && fi.total_number_of_free_pages != 0xFFFFFFFFul) {
            size_t free_bytes = (size_t)fi.total_number_of_free_pages * 4096;
            s_text_budget = free_bytes / 8 / 4;          // an eighth of it, 4 bytes a pixel
        }
        if (s_text_budget < (192u << 10)) s_text_budget = 192u << 10;   // (two screens' text at 640x480)
        if (s_text_budget > (1u << 20)) s_text_budget = 1u << 20;       // (4 MB)
    }
    g_iv_busy_hook = show_busy;      // "Loading ..." while felixchirp/ opens a file
    log_memory("at start");
    int want_w = 0, want_h = 0, want_bpp = 0;
    const char *start = nullptr;
    bool show_modes = false;
    const char *env = getenv("CHIRP_VIDEO");
    if (env) parse_mode(env, &want_w, &want_h, &want_bpp);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-g") || !strcmp(a, "/g")) && i + 1 < argc) {
            if (!parse_mode(argv[++i], &want_w, &want_h, &want_bpp)) { fprintf(stderr, "CHIRP: -g WIDTHxHEIGHT[xBITS]\n"); return 1; }
        } else if ((!strcmp(a, "-f") || !strcmp(a, "/f")) && i + 1 < argc) {
            g_font_size = atoi(argv[++i]);
            if (g_font_size < 8) g_font_size = 8;
            if (g_font_size > 48) g_font_size = 48;
        } else if (!strcmp(a, "-l") || !strcmp(a, "/l")) {
            show_modes = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "/?") || !strcmp(a, "-?") || !strcmp(a, "--help")) {
            usage(); return 0;
        } else if (a[0] == '-') {
            usage(); return 1;
        } else {
            start = a;
        }
    }

    // Where to start, and the file to open (if a file was given)
    std::string dir, file;
    if (start) {
        std::string s = dos_path(start);
        struct stat st;
        if (stat(s.c_str(), &st) != 0) { fprintf(stderr, "CHIRP: no %s\n", start); return 1; }
        if (S_ISDIR(st.st_mode)) dir = s;
        else {
            size_t sl = s.find_last_of("\\:");
            if (sl == std::string::npos) { file = s; }
            else { dir = s.substr(0, sl + 1); file = s.substr(sl + 1); }
        }
    }
    if (dir.empty()) {
        char cwd[1024];
        dir = getcwd(cwd, sizeof cwd) ? cwd : "C:\\";
    }
    if (!dir.empty() && dir[0] != '\\' && !(dir.size() > 1 && dir[1] == ':')) {   // relative: from the current one
        char cwd[1024];
        if (getcwd(cwd, sizeof cwd)) dir = std::string(cwd) + "\\" + dir;
    }
    dir = dos_path(dir);

    if (!font_init()) { fprintf(stderr, "CHIRP: no font\n"); return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "CHIRP: SDL: %s\n", SDL_GetError()); return 1; }
    if (show_modes) { list_modes(); SDL_Quit(); return 0; }
    SDL_DisplayMode mode;
    if (!pick_mode(want_w, want_h, want_bpp, &mode) && !pick_mode(0, 0, 0, &mode)) {
        fprintf(stderr, "CHIRP: no VESA mode of 640x480 or more in 15 bits or more\n");
        SDL_Quit();
        return 1;
    }
    // The mode is chosen before the window is shown, so it's set once: SDL's
    // DOS driver clears the screen when it sets a mode, and a second mode set
    // after the first frame left the screen black.
    g_sdl_window = SDL_CreateWindow("Felix Chirp", mode.w, mode.h, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIDDEN);
    if (!g_sdl_window) { fprintf(stderr, "CHIRP: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetWindowFullscreenMode(g_sdl_window, &mode);
    SDL_ShowWindow(g_sdl_window);
    SDL_SyncWindow(g_sdl_window);
    g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, nullptr);
    if (!g_sdl_renderer) { SDL_Quit(); fprintf(stderr, "CHIRP: renderer: %s\n", SDL_GetError()); return 1; }
    SDL_ShowCursor();
    // Sound: SDL's Sound Blaster driver reads BLASTER. Without one, pictures
    // and text still work; songs say there's no sound card.
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    iv_ensure_mixer();

    log_memory("with the screen and sound open");
    int win_w = mode.w, win_h = mode.h;
    s_win_w = win_w; s_win_h = win_h;
    iv_open(false, win_w, win_h);
    strncpy(g_iv.path, dir.c_str(), sizeof(g_iv.path) - 1);
    iv_refresh();
    if (!file.empty()) {
        for (size_t i = 0; i < g_iv.entries.size(); i++)
            if (!strcasecmp(g_iv.entries[i].name, file.c_str())) { g_iv.selected = (int)i; iv_enter_selected(); break; }
    }

    uint64_t last = SDL_GetTicks(), last_frame = 0, started = last;
    bool need = true, quit = false, pointer_moved = false;
    while (!quit && g_iv.visible) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                // Ctrl+Q / F10 quit from anywhere; Esc closes Chirp
                // (after leaving fullscreen or a text file) as in Felix Terminal.
                if ((e.key.key == SDLK_Q && (e.key.mod & SDL_KMOD_CTRL)) || e.key.key == SDLK_F10) { quit = true; break; }
                // F2 / Shift+F2: the next / previous screen mode
                if (e.key.key == SDLK_F2) {
                    std::vector<SDL_DisplayMode> ms = usable_modes();
                    if (!ms.empty()) {
                        int cur = -1;
                        for (size_t i = 0; i < ms.size(); i++) if (ms[i].w == win_w && ms[i].h == win_h) cur = (int)i;
                        int k = (int)ms.size();
                        int next = (e.key.mod & SDL_KMOD_SHIFT) ? (cur < 0 ? k - 1 : (cur + k - 1) % k) : (cur + 1) % k;
                        if (set_mode(ms[(size_t)next], &win_w, &win_h)) { s_win_w = win_w; s_win_h = win_h; }
                        else
                            snprintf(g_iv.error, sizeof g_iv.error, "Can't set %dx%d: %s", ms[(size_t)next].w, ms[(size_t)next].h, SDL_GetError());
                    }
                    need = true;
                    break;
                }
                // Alt+letter: that drive's root (the root's listing has the drives too)
                if ((e.key.mod & SDL_KMOD_ALT) && e.key.key >= SDLK_A && e.key.key <= SDLK_Z) {
                    char root[4] = { (char)('A' + (e.key.key - SDLK_A)), ':', '\\', 0 };
                    struct stat st;
                    if (stat(root, &st) == 0) {
                        iv_stop_audio();
                        g_iv.in_zip = false;
                        g_iv.zip_file[0] = '\0';
                        strcpy(g_iv.path, root);
                        g_iv.selected = 0;
                        g_iv.scroll_top = 0;
                        iv_refresh();
                    } else {
                        snprintf(g_iv.error, sizeof g_iv.error, "Can't read %c: (no disk?)", root[0]);
                    }
                    need = true;
                    break;
                }
                iv_keydown(e.key.key);
                need = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                iv_mousedown((int)e.button.x, (int)e.button.y, e.button.button, win_w, win_h);
                need = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                iv_mouseup((int)e.button.x, (int)e.button.y, e.button.button);
                need = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (iv_mousemotion((int)e.motion.x, (int)e.motion.y, win_w, win_h)) need = true;
                // SDL draws the pointer itself, as a frame is shown: it only
                // moves when one is (below, up to 60 a second)
                pointer_moved = true;
                break;
            case SDL_EVENT_WINDOW_SHOWN: case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED: case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
                need = true;                       // (the screen may have been cleared)
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                iv_mousewheel((int)e.wheel.mouse_x, (int)e.wheel.mouse_y,
                              e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0, win_w, win_h);
                need = true;
                break;
            default: break;
            }
        }
        if (quit || !g_iv.visible) break;

        // ".." from C:\GAMES leaves "C:": make it the root, C:\ .
        size_t pl = strlen(g_iv.path);
        if (!g_iv.in_zip && ((pl == 2 && g_iv.path[1] == ':') || (pl == 1 && g_iv.path[0] == '\\'))) {
            if (pl == 1) { snprintf(g_iv.path, sizeof g_iv.path, "%s", dir.substr(0, 2).c_str()); }
            strcat(g_iv.path, "\\");
            iv_refresh();
            need = true;
        }

        uint64_t now = SDL_GetTicks();
        // Chirp times a song (the lyrics, the visualizer, the clock shown)
        // from when it started. Here that's steered to the sound as heard
        // (chirp_music_position): a little each frame so it stays smooth, at
        // once when far out (a seek, the main loop busy for a while).
        if (g_iv.audio_playing && !g_iv.audio_paused && g_iv.music) {
            double heard = chirp_music_position();
            if (heard >= 0) {
                double ours = (now - g_iv.audio_start_ticks) / 1000.0;
                double err = ours - heard;
                if (err > 0.25 || err < -0.25) g_iv.audio_start_ticks = now - heard * 1000.0;
                else g_iv.audio_start_ticks += err * 1000.0 * 0.05;
                if (g_chirp_log) {
                    static uint64_t le;
                    if (now - le >= 2000) {
                        le = now;
                        fprintf(g_chirp_log, "song at %.2f s heard, %.3f s by Chirp's clock (latency %.3f)\n", heard, ours,
                                chirp_audio_latency());
                        fflush(g_chirp_log);
                    }
                }
            }
        }
        { static uint64_t lm; if (g_chirp_log && now - lm >= 5000) { lm = now; log_memory("now"); } }
        iv_tick((now - last) / 1000.0);
        last = now;
        bool animating = (g_iv.audio_playing && !g_iv.audio_paused) || g_iv.cdg_display || g_iv.slideshow_active;
        // The first two seconds always, then at least twice a second: a frame
        // the video BIOS dropped (a mode set clears the screen) doesn't stay lost.
        if (now - started < 2000 || now - last_frame >= 500) animating = true;
        // Animations at up to 60 frames a second, input at once. DOS's
        // threads are cooperative: SDL's audio thread (the Sound Blaster's
        // mixing) runs only while this one waits, so it always waits a
        // little, and longer the longer a frame took to draw.
        if (need || ((animating || pointer_moved) && now - last_frame >= 16)) {
            pointer_moved = false;
            s_t_rect = s_t_text = 0; s_n_rect = s_n_text = s_n_glyph = 0;
            s_frame++;
            forget_old_texts();
            iv_render(win_w, win_h);
            uint64_t drawn = SDL_GetTicks();
            SDL_RenderPresent(g_sdl_renderer);
            uint64_t took = SDL_GetTicks() - now;
            if (g_chirp_log) {
                static int n;
                if (++n % 25 == 0) {
                    fprintf(g_chirp_log, "frame %llu ms (drawing %llu, showing %llu): %ld rects %.1f ms, %ld texts (%ld drawn) %.1f ms\n",
                            (unsigned long long)took, (unsigned long long)(drawn - now), (unsigned long long)(now + took - drawn),
                            s_n_rect, s_t_rect * 1000.0 / UCLOCKS_PER_SEC, s_n_text, s_n_glyph, s_t_text * 1000.0 / UCLOCKS_PER_SEC);
                    fflush(g_chirp_log);
                }
            }
            last_frame = now;
            need = false;
            SDL_Delay(took > 40 ? 10 : 1);
        } else {
            SDL_Delay(animating ? 1 : 5);
        }
    }
    if (g_iv.visible) iv_close();
    Mix_CloseAudio();
    SDL_Quit();
    return 0;
}
