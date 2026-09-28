// kitty_graphics.cpp — Kitty Graphics Protocol implementation
//
// Supported:
//   Transmission:  direct (base64 inline), single and multi-chunk
//   Formats:       f=32 RGBA, f=24 RGB, f=100 PNG (via stb_image)
//   Actions:       a=T (transmit+display), a=p (put/display), a=d (delete)
//   Placement:     virtual cell grid (c= cols, r= rows) or pixel (w=,h=)
//   Quiet:         q=1/2 suppresses OK/error responses
//
// Not yet supported:
//   Shared memory / file transmission (a=f, t=s/f)
//   Animation frames
//   Unicode placeholder rendering

#include "kitty_graphics.h"
#include "gl_renderer.h"
#include "sdl_renderer.h"
#include "term_pty.h"
#include "gl_terminal.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ============================================================================
// PNG decode via libpng — same dependency used for encode, works on all platforms
// ============================================================================
#include <png.h>

struct PngReadCtx {
    const uint8_t *data;
    int            len;
    int            pos;
};

static void png_mem_read(png_structp ps, png_bytep out, png_size_t len) {
    auto *ctx = (PngReadCtx*)png_get_io_ptr(ps);
    int remaining = ctx->len - ctx->pos;
    if ((int)len > remaining) len = (png_size_t)remaining;
    memcpy(out, ctx->data + ctx->pos, len);
    ctx->pos += (int)len;
}

// Decode any PNG from memory into a freshly malloc'd RGBA buffer.
// Returns nullptr on failure. Caller must free() the result.
static uint8_t *decode_png(const uint8_t *src, int srclen, int *w_out, int *h_out) {
    if (srclen < 8 || png_sig_cmp((png_bytep)src, 0, 8)) return nullptr;

    png_structp ps = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!ps) return nullptr;
    png_infop pi = png_create_info_struct(ps);
    if (!pi) { png_destroy_read_struct(&ps, nullptr, nullptr); return nullptr; }

    if (setjmp(png_jmpbuf(ps))) {
        png_destroy_read_struct(&ps, &pi, nullptr);
        return nullptr;
    }

    PngReadCtx ctx = { src, srclen, 0 };
    png_set_read_fn(ps, &ctx, png_mem_read);
    png_read_info(ps, pi);

    int w  = (int)png_get_image_width(ps, pi);
    int h  = (int)png_get_image_height(ps, pi);
    int ct = png_get_color_type(ps, pi);
    int bd = png_get_bit_depth(ps, pi);

    // Normalise to 8-bit RGBA
    if (bd == 16)                       png_set_strip_16(ps);
    if (ct == PNG_COLOR_TYPE_PALETTE)   png_set_palette_to_rgb(ps);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(ps);
    if (png_get_valid(ps, pi, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(ps);
    if (ct == PNG_COLOR_TYPE_RGB  ||
        ct == PNG_COLOR_TYPE_GRAY ||
        ct == PNG_COLOR_TYPE_PALETTE)   png_set_filler(ps, 0xFF, PNG_FILLER_AFTER);
    if (ct == PNG_COLOR_TYPE_GRAY ||
        ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(ps);
    png_read_update_info(ps, pi);

    uint8_t *pixels = (uint8_t*)malloc((size_t)w * h * 4);
    if (!pixels) { png_destroy_read_struct(&ps, &pi, nullptr); return nullptr; }

    std::vector<png_bytep> rows((size_t)h);
    for (int r = 0; r < h; r++) rows[r] = pixels + r * w * 4;
    png_read_image(ps, rows.data());
    png_read_end(ps, nullptr);
    png_destroy_read_struct(&ps, &pi, nullptr);

    *w_out = w; *h_out = h;
    return pixels;
}


// ============================================================================
// TEXTURED QUAD SHADER
// ============================================================================

static const char *IMG_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec2 tc;\n"
    "uniform mat4 proj;\n"
    "out vec2 vTC;\n"
    "void main(){ gl_Position = proj * vec4(pos,0,1); vTC = tc; }\n";

static const char *IMG_FS =
    "#version 330 core\n"
    "in vec2 vTC;\n"
    "out vec4 frag;\n"
    "uniform sampler2D img;\n"
    "void main(){ frag = texture(img, vTC); }\n";

static GLuint s_img_prog  = 0;
static GLuint s_img_vao   = 0;
static GLuint s_img_vbo   = 0;
static GLint  s_img_proj  = -1;
static GLint  s_img_tex   = -1;

// ============================================================================
// BASE64 DECODE
// ============================================================================

static const int8_t B64TAB[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static std::vector<uint8_t> b64_decode(const char *src, int slen) {
    std::vector<uint8_t> out;
    out.reserve((slen * 3) / 4 + 4);
    uint32_t acc = 0;
    int bits = 0;
    for (int i = 0; i < slen; i++) {
        int8_t v = B64TAB[(uint8_t)src[i]];
        if (v < 0) continue;  // skip whitespace / padding
        acc = (acc << 6) | (uint8_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
            acc &= (1u << bits) - 1;
        }
    }
    return out;
}

// ============================================================================
// IMAGE STORE
// ============================================================================

// One frame of an animated image.
struct KittyFrame {
    GLuint       tex         = 0;
    SDL_Texture *sdl_tex     = nullptr;
    int    pw = 0, ph  = 0;
    int    duration_ms = 100;
};

struct KittyImage {
    uint32_t     id;
    GLuint       tex        = 0;
    SDL_Texture *sdl_tex    = nullptr;
    int      pw, ph;

    std::vector<KittyFrame> frames;
    bool animated = false;
};

struct KittyPlacement {
    uint32_t image_id;
    uint32_t placement_id; // p= (0 = unnamed)
    int      x_cell, y_cell;   // top-left cell in terminal grid
    int      src_x, src_y;     // source pixel offset within image
    int      src_w, src_h;     // source pixel region (0=full)
    int      cols, rows;       // display size in cells (0=auto)
    int      z_index;          // z= layering (-inf..+inf, 0=default over text)
    bool     alt;              // placed on the alternate screen (vim, less...)
};

// Per-image animation playback state (one per unique image_id, shared across
// all placements showing that image — matches timg/kitty behaviour).
enum class AnimState { STOPPED, RUNNING, ONCE };

struct KittyAnimCtrl {
    AnimState state       = AnimState::STOPPED;
    int       cur_frame   = 0;
    double    accum_ms    = 0.0;  // accumulated time since last frame advance
    int       loops_left  = 0;    // 0 = infinite
    int       gap_ms      = 0;    // override frame gap (a=a z=); 0 = use per-frame duration
};

// In-progress chunked transmission
struct KittyChunk {
    uint32_t         id;
    uint32_t         placement_id;
    int              fmt;   // 32,24,100
    int              pw, ph;
    std::vector<uint8_t> data;
    bool             display_when_done;
    // display params from first chunk
    int x_cell, y_cell, cols, rows, z_index;
    int quiet;
    // animation frame params (a=f)
    int frame_number;       // r= target frame number (1-based; 0=append new frame)
    int frame_duration_ms;  // z= duration for this frame (ms); 0 = use image default
    int comp_x, comp_y;     // x=,y= pixel offset when compositing onto base frame
    int comp_base_frame;    // c= frame to composite onto (0 = use background colour)
    bool is_frame;          // true when a=f
};

// Per-terminal state pointer stored in a map keyed by Terminal*
struct KittyTermState {
    std::vector<KittyPlacement> placements;
    KittyChunk                  pending;   // active chunked transfer
    bool                        has_pending = false;
};

// Animation state lives globally keyed by image id so placements share it
static std::unordered_map<uint32_t, KittyAnimCtrl> s_anim;

static std::unordered_map<uint32_t, KittyImage>  s_images;   // id → image
static std::unordered_map<Terminal*, KittyTermState> s_terms;

static uint32_t s_next_auto_id = 1;

// ============================================================================
// HELPERS
// ============================================================================

static GLuint upload_texture(const uint8_t *pixels, int w, int h, bool has_alpha) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum fmt = has_alpha ? GL_RGBA : GL_RGB;
    // Source buffers are tightly packed (no row padding), but GL's default
    // GL_UNPACK_ALIGNMENT of 4 assumes rows start on 4-byte boundaries. For
    // RGB data (3 bytes/pixel) with a width not divisible by 4, that mismatch
    // causes each row to be read from the wrong offset, producing diagonal/
    // banding corruption. Set alignment to 1 to disable the assumption.
    GLint prev_alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// Upload pixels as an SDL_Texture (always RGBA, ABGR8888 matches stb/libpng output)
static SDL_Texture *upload_sdl_texture(const uint8_t *pixels, int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(g_sdl_renderer,
        SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, pixels, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

// Upload RGB pixels (expand to RGBA)
static SDL_Texture *upload_sdl_texture_rgb(const uint8_t *pixels, int w, int h) {
    std::vector<uint8_t> rgba(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i*4+0] = pixels[i*3+0];
        rgba[i*4+1] = pixels[i*3+1];
        rgba[i*4+2] = pixels[i*3+2];
        rgba[i*4+3] = 0xFF;
    }
    return upload_sdl_texture(rgba.data(), w, h);
}

static void free_tex(GLuint gl, SDL_Texture *sdl) {
    if (g_use_sdl_renderer) { if (sdl) SDL_DestroyTexture(sdl); }
    else                    { if (gl)  glDeleteTextures(1, &gl); }
}

// Build image from raw/decoded data. Fills either gl_tex or sdl_tex depending on path.
// Returns true on success.
static bool build_image(const std::vector<uint8_t> &raw, int fmt, int pw, int ph,
                        GLuint *gl_tex_out, SDL_Texture **sdl_tex_out,
                        int *w_out, int *h_out) {
    if (fmt == 100) {
        int w = 0, h = 0;
        uint8_t *dec = decode_png(raw.data(), (int)raw.size(), &w, &h);
        if (!dec) { SDL_Log("[Kitty] PNG decode failed\n"); return false; }
        if (g_use_sdl_renderer) *sdl_tex_out = upload_sdl_texture(dec, w, h);
        else                    *gl_tex_out   = upload_texture(dec, w, h, true);
        free(dec);
        *w_out = w; *h_out = h;
        return g_use_sdl_renderer ? (*sdl_tex_out != nullptr) : (*gl_tex_out != 0);
    }
    if (fmt == 32) {
        int expected = pw * ph * 4;
        if ((int)raw.size() < expected) { SDL_Log("[Kitty] f=32 data too short\n"); return false; }
        if (g_use_sdl_renderer) *sdl_tex_out = upload_sdl_texture(raw.data(), pw, ph);
        else                    *gl_tex_out   = upload_texture(raw.data(), pw, ph, true);
        *w_out = pw; *h_out = ph;
        return g_use_sdl_renderer ? (*sdl_tex_out != nullptr) : (*gl_tex_out != 0);
    }
    if (fmt == 24) {
        int expected = pw * ph * 3;
        if ((int)raw.size() < expected) { SDL_Log("[Kitty] f=24 data too short\n"); return false; }
        if (g_use_sdl_renderer) *sdl_tex_out = upload_sdl_texture_rgb(raw.data(), pw, ph);
        else                    *gl_tex_out   = upload_texture(raw.data(), pw, ph, false);
        *w_out = pw; *h_out = ph;
        return g_use_sdl_renderer ? (*sdl_tex_out != nullptr) : (*gl_tex_out != 0);
    }
    SDL_Log("[Kitty] unsupported format %d\n", fmt);
    return false;
}

static void send_response(Terminal *t, uint32_t id, int quiet, bool ok, const char *msg) {
    if (quiet >= 2) return;
    if (quiet == 1 && ok) return;
    char buf[128];
    if (ok) {
        int n = snprintf(buf, sizeof(buf), "\x1b_Gi=%u;OK\x1b\\", id);
        term_write(t, buf, n);
    } else {
        int n = snprintf(buf, sizeof(buf), "\x1b_Gi=%u;ERROR:%s\x1b\\", id, msg ? msg : "unknown");
        term_write(t, buf, n);
    }
}

// ============================================================================
// PAYLOAD PARSER
// ============================================================================

// Parse "key=value,key=value,...;base64data" or "key=value,key=value"
struct ApcParams {
    int    a    = 0;    // action: 'T'=transmit+display,'p'=put,'d'=delete,'t'=transmit,'q'=query,'f'=frame,'c'=compose,'a'=animate
    int    f    = 32;   // format: 32=RGBA,24=RGB,100=PNG
    int    m    = 0;    // more chunks: 0=last, 1=more coming
    uint32_t i  = 0;    // image id (0=auto-assign)
    uint32_t p  = 0;    // placement id
    int    s    = 0;    // pixel width
    int    v    = 0;    // pixel height
    int    S    = 0;    // data size hint (ignored)
    int    x    = 0;    // source x pixel / composition x offset (a=f)
    int    y    = 0;    // source y pixel / composition y offset (a=f)
    int    w    = 0;    // source pixel width
    int    h    = 0;    // source pixel height
    int    c    = 0;    // display columns / base frame for composition (a=f)
    int    r    = 0;    // display rows / target frame number (a=f: 1-based, 0=append)
    int    X    = 0;    // pixel x offset within cell
    int    Y    = 0;    // pixel y offset within cell
    int    z    = 0;    // z-index / frame duration ms (a=f/a=a)
    int    q    = 0;    // quiet: 0=always respond, 1=ok silent, 2=all silent
    int    d    = 0;    // delete target: 'a'=all,'i'=image,'p'=placement,'z'=zindex,'c'/'r'=col/row
    int    t_   = 'd';  // transmission medium: 'd'=direct,'f'=file,'s'=shm
    // Animation control (a=a)
    int    A_s  = 0;    // s= animation state: 1=stop, 2=run (loop), 3=run once
    int    A_v  = 0;    // v= loop count: 0=infinite, N=stop after N loops
    int    A_c  = 0;    // c= (a=a) set current frame (1-based)
    const char *payload = nullptr;
    int    payload_len  = 0;
};

static ApcParams parse_apc(const char *data, int len) {
    ApcParams p;
    // Find the semicolon separating keys from data
    int semi = -1;
    for (int i = 0; i < len; i++) {
        if (data[i] == ';') { semi = i; break; }
    }
    int kv_end = (semi >= 0) ? semi : len;
    if (semi >= 0) {
        p.payload     = data + semi + 1;
        p.payload_len = len - semi - 1;
    }
    // Parse key=value pairs
    const char *kp = data;
    const char *end = data + kv_end;
    while (kp < end) {
        const char *eq = nullptr;
        const char *comma = kp;
        while (comma < end && *comma != ',') comma++;
        for (const char *c = kp; c < comma; c++) {
            if (*c == '=') { eq = c; break; }
        }
        if (eq) {
            char key = *kp;
            int  val = atoi(eq + 1);
            // handle string value for 'a' and 't'
            switch (key) {
            case 'a': p.a    = *(eq+1); break;  // char value
            case 't': p.t_   = *(eq+1); break;
            case 'd': p.d    = *(eq+1); break;  // delete target char
            case 'f': p.f    = val; break;
            case 'm': p.m    = val; break;
            case 'i': p.i    = (uint32_t)val; break;
            case 'p': p.p    = (uint32_t)val; break;
            case 's': p.s    = val; break;
            case 'v': p.v    = val; break;
            case 'S': p.S    = val; break;
            case 'x': p.x    = val; break;
            case 'y': p.y    = val; break;
            case 'w': p.w    = val; break;
            case 'h': p.h    = val; break;
            case 'c': p.c    = val; break;
            case 'r': p.r    = val; break;
            case 'X': p.X    = val; break;
            case 'Y': p.Y    = val; break;
            case 'z': p.z    = val; break;
            case 'q': p.q    = val; break;
            }
        }
        kp = comma + 1;
    }
    return p;
}

// ============================================================================
// kitty_handle_apc — main entry point
// ============================================================================

void kitty_handle_apc(Terminal *t, const char *payload, int len) {
    // Must start with "G"
    if (len < 1 || payload[0] != 'G') return;
    payload++; len--;

    ApcParams p = parse_apc(payload, len);
    KittyTermState &ts = s_terms[t];

    // ---- QUERY action (a=q) ----
    // Per spec: if a payload is attached, the terminal must attempt to
    // decode it (without storing/displaying) and reply OK/error based on
    // whether decoding succeeds — this is the standard capability probe
    // well-behaved clients send before committing to the protocol. With
    // no payload, fall back to checking whether the id is already known.
    if (p.a == 'q') {
        if (p.payload_len > 0 && p.m == 0) {
            std::vector<uint8_t> raw = b64_decode(p.payload, p.payload_len);
            GLuint gl_tex = 0; SDL_Texture *sdl_tex = nullptr;
            int w = 0, h = 0;
            bool ok = build_image(raw, p.f, p.s, p.v, &gl_tex, &sdl_tex, &w, &h);
            free_tex(gl_tex, sdl_tex);
            send_response(t, p.i, p.q, ok, ok ? nullptr : "decode failed");
            return;
        }
        bool found = (p.i != 0 && s_images.count(p.i) > 0);
        send_response(t, p.i, p.q, found, found ? nullptr : "image not found");
        return;
    }

    // ---- DELETE action (a=d) ----
    if (p.a == 'd' || p.a == 'D') {
        auto &pv = ts.placements;
        int dtarget = p.d ? p.d : 'a';  // default: delete all visible
        switch (dtarget) {
        case 'a': case 'A':
            // Delete all placements (upper=also free image data)
            pv.clear();
            if (p.d == 'A') {
                if (p.i) {
                    auto it=s_images.find(p.i);
                    if (it!=s_images.end()) {
                        for (auto &fr : it->second.frames) free_tex(fr.tex, fr.sdl_tex);
                        free_tex(it->second.tex, it->second.sdl_tex);
                        s_images.erase(it);
                    }
                } else {
                    for (auto &kv : s_images) {
                        for (auto &fr : kv.second.frames) free_tex(fr.tex, fr.sdl_tex);
                        free_tex(kv.second.tex, kv.second.sdl_tex);
                    }
                    s_images.clear();
                }
            }
            break;
        case 'i': case 'I':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.image_id==p.i;}),pv.end());
            if (p.d == 'I') {
                auto it=s_images.find(p.i);
                if (it!=s_images.end()) {
                    for (auto &fr : it->second.frames) free_tex(fr.tex, fr.sdl_tex);
                    free_tex(it->second.tex, it->second.sdl_tex);
                    s_images.erase(it);
                }
            }
            break;
        case 'p': case 'P':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){
                return pl.image_id==p.i && pl.placement_id==p.p; }),pv.end());
            break;
        case 'z': case 'Z':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.z_index==p.z;}),pv.end());
            break;
        case 'c': case 'C':
            // Delete placements whose column range contains cursor col
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){
                return t->cur_col >= pl.x_cell && t->cur_col < pl.x_cell + (pl.cols?pl.cols:1);
            }),pv.end());
            break;
        case 'r': case 'R':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){
                return t->cur_row >= pl.y_cell && t->cur_row < pl.y_cell + (pl.rows?pl.rows:1);
            }),pv.end());
            break;
        case 'x': case 'X':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.x_cell==p.x;}),pv.end());
            break;
        case 'y': case 'Y':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.y_cell==p.y;}),pv.end());
            break;
        case 'n': case 'N':
            // Delete newest (last) placement
            if (!pv.empty()) pv.pop_back();
            break;
        case 'o': case 'O':
            // Delete oldest (first) placement
            if (!pv.empty()) pv.erase(pv.begin());
            break;
        default:
            pv.clear();
            break;
        }
        return;
    }

    // ---- TRANSMIT or TRANSMIT+DISPLAY ----
    bool is_transmit = (p.a == 'T' || p.a == 't' || p.a == 0);
    bool is_display  = (p.a == 'T' || p.a == 'p' || p.a == 0);
    bool is_frame    = (p.a == 'f');   // a=f: animation frame
    bool is_compose  = (p.a == 'c');   // a=c: compose frame
    bool is_animate  = (p.a == 'a');   // a=a: animate control

    // ---- ANIMATE control (a=a) — no payload required ----
    if (is_animate) {
        KittyAnimCtrl &ac = s_anim[p.i];
        if (p.A_s == 1) {
            ac.state = AnimState::STOPPED;
        } else if (p.A_s == 2) {
            ac.state      = AnimState::RUNNING;
            ac.loops_left = p.A_v;
            if (ac.loops_left < 0) ac.loops_left = 0;
        } else if (p.A_s == 3) {
            ac.state      = AnimState::ONCE;
            ac.cur_frame  = 0;
            ac.accum_ms   = 0.0;
        }
        if (p.z > 0) ac.gap_ms = p.z;
        if (p.c > 0) {  // c= set current frame (1-based in spec, store 0-based)
            auto iit = s_images.find(p.i);
            if (iit != s_images.end() && !iit->second.frames.empty()) {
                int fi = p.c - 1;
                if (fi >= 0 && fi < (int)iit->second.frames.size())
                    ac.cur_frame = fi;
            }
        }
        send_response(t, p.i, p.q, true, nullptr);
        return;
    }

    // ---- COMPOSE (a=c) — recomposite frame onto base; no pixel data needed ----
    // We handle this lazily: the frame already has its own texture so we just
    // note the composition. Full CPU-side compositing would be done here if needed.
    // For timg animated GIFs the frames arrive already pre-composited, so this
    // is mostly a no-op stub that sends a success response.
    if (is_compose) {
        send_response(t, p.i, p.q, true, nullptr);
        return;
    }

    if ((is_transmit || is_frame) && p.payload_len > 0) {
        if (ts.has_pending) {
            if (p.i != 0 && p.i != ts.pending.id) {
                // Different id — abandon previous
                ts.has_pending = false;
            }
        }
        if (!ts.has_pending) {
            ts.pending = {};
            ts.pending.id           = p.i ? p.i : s_next_auto_id++;
            ts.pending.placement_id = p.p;
            ts.pending.fmt          = p.f;
            ts.pending.pw           = p.s;
            ts.pending.ph           = p.v;
            ts.pending.display_when_done = is_display && !is_frame;
            ts.pending.x_cell  = t->cur_col;
            ts.pending.y_cell  = t->cur_row;
            ts.pending.is_frame = is_frame;
            if (is_frame) {
                // a=f: r=frame(1-based,0=append), z=duration_ms, x/y=comp offset, c=base_frame
                ts.pending.frame_number      = p.r;
                ts.pending.frame_duration_ms = p.z;
                ts.pending.comp_x            = p.x;
                ts.pending.comp_y            = p.y;
                ts.pending.comp_base_frame   = p.c;
                ts.pending.cols    = 0;
                ts.pending.rows    = 0;
                ts.pending.z_index = 0;
            } else {
                ts.pending.cols    = p.c;
                ts.pending.rows    = p.r;
                ts.pending.z_index = p.z;
                ts.pending.frame_number      = 0;
                ts.pending.frame_duration_ms = 100;
            }
            ts.pending.quiet   = p.q;
            ts.has_pending     = true;
        }
        // Decode and append base64 payload
        std::vector<uint8_t> chunk = b64_decode(p.payload, p.payload_len);
        auto &pd = ts.pending.data;
        pd.insert(pd.end(), chunk.begin(), chunk.end());

        if (p.m == 0) {
            // Last chunk — build texture
            uint32_t img_id = ts.pending.id;

            // ---- a=f: append/replace an animation frame ----
            if (ts.pending.is_frame) {
                GLuint       frame_gl  = 0;
                SDL_Texture *frame_sdl = nullptr;
                int fw = 0, fh = 0;
                bool ok = false;
                if (ts.pending.fmt == 100) {
                    uint8_t *dec = decode_png(
                        ts.pending.data.data(), (int)ts.pending.data.size(), &fw, &fh);
                    if (dec) {
                        if (g_use_sdl_renderer) frame_sdl = upload_sdl_texture(dec, fw, fh);
                        else                    frame_gl   = upload_texture(dec, fw, fh, true);
                        free(dec);
                        ok = g_use_sdl_renderer ? (frame_sdl != nullptr) : (frame_gl != 0);
                    } else SDL_Log("[Kitty] a=f PNG decode failed\n");
                } else {
                    ok = build_image(ts.pending.data, ts.pending.fmt,
                                     ts.pending.pw, ts.pending.ph,
                                     &frame_gl, &frame_sdl, &fw, &fh);
                }

                if (ok) {
                    KittyImage &img = s_images[img_id];
                    img.id = img_id;
                    img.animated = true;

                    if (!img.tex && !img.sdl_tex) {
                        img.tex     = frame_gl;
                        img.sdl_tex = frame_sdl;
                        img.pw = fw; img.ph = fh;
                        if (img.frames.empty()) {
                            KittyFrame f0;
                            f0.tex = frame_gl; f0.sdl_tex = frame_sdl;
                            f0.pw = fw; f0.ph = fh;
                            f0.duration_ms = ts.pending.frame_duration_ms > 0
                                           ? ts.pending.frame_duration_ms : 100;
                            img.frames.push_back(f0);
                        }
                    } else {
                        int fn  = ts.pending.frame_number;
                        int dur = ts.pending.frame_duration_ms > 0
                                ? ts.pending.frame_duration_ms : 100;
                        KittyFrame fr;
                        fr.tex = frame_gl; fr.sdl_tex = frame_sdl;
                        fr.pw = fw; fr.ph = fh;
                        fr.duration_ms = dur;
                        if (fn == 0 || fn > (int)img.frames.size()) {
                            img.frames.push_back(fr);
                        } else {
                            int idx = fn - 1;
                            KittyFrame &old = img.frames[idx];
                            if (old.tex != img.tex || old.sdl_tex != img.sdl_tex)
                                free_tex(old.tex, old.sdl_tex);
                            img.frames[idx] = fr;
                        }
                    }

                    KittyAnimCtrl &ac = s_anim[img_id];
                    if (ac.state == AnimState::STOPPED && img.frames.size() > 1)
                        ac.state = AnimState::RUNNING;

                    send_response(t, img_id, ts.pending.quiet, true, nullptr);
                } else {
                    send_response(t, img_id, ts.pending.quiet, false, "frame decode failed");
                }
                ts.has_pending = false;
                return;
            }

            // ---- Normal transmit: replace whole image ----
            {
                auto it = s_images.find(img_id);
                if (it != s_images.end()) {
                    for (auto &fr : it->second.frames)
                        free_tex(fr.tex, fr.sdl_tex);
                    free_tex(it->second.tex, it->second.sdl_tex);
                    s_images.erase(it);
                }
                s_anim.erase(img_id);
            }

            KittyImage img = {};
            img.id = img_id;

            {
                GLuint       gl_tex  = 0;
                SDL_Texture *sdl_tex = nullptr;
                int w = 0, h = 0;
                bool ok = build_image(ts.pending.data, ts.pending.fmt,
                                      ts.pending.pw, ts.pending.ph,
                                      &gl_tex, &sdl_tex, &w, &h);
                if (ok) {
                    img.pw = w; img.ph = h;
                    img.tex = gl_tex; img.sdl_tex = sdl_tex;
                }
            }

            if (img.tex || img.sdl_tex) {
                s_images[img_id] = img;
                send_response(t, img_id, ts.pending.quiet, true, nullptr);

                if (ts.pending.display_when_done) {
                    KittyPlacement pl = {};
                    pl.image_id    = img_id;
                    pl.placement_id = ts.pending.placement_id;
                    pl.x_cell      = ts.pending.x_cell;
                    pl.y_cell      = ts.pending.y_cell;
                    pl.src_x       = p.x; pl.src_y = p.y;
                    pl.src_w       = p.w; pl.src_h = p.h;
                    pl.cols        = ts.pending.cols;
                    pl.rows        = ts.pending.rows;
                    pl.z_index     = ts.pending.z_index;
                    pl.alt         = t->in_alt_screen;
                    ts.placements.push_back(pl);

                    // Advance cursor past image using newline() so the scroll
                    // region fires correctly when the image is near the bottom.
                    int rows_used = pl.rows ? pl.rows :
                        (img.ph > 0 && t->cell_h > 0) ?
                            (int)((img.ph + (int)t->cell_h - 1) / (int)t->cell_h) : 1;
                    for (int r = 0; r < rows_used; r++)
                        term_newline(t);
                    t->cur_col = 0;
                }
            } else {
                send_response(t, img_id, ts.pending.quiet, false, "decode failed");
            }
            ts.has_pending = false;
        }
    } else if (is_display && !is_transmit) {
        // a=p: place an already-uploaded image
        if (p.i == 0 || s_images.find(p.i) == s_images.end()) {
            send_response(t, p.i, p.q, false, "image not found");
            return;
        }
        KittyPlacement pl = {};
        pl.image_id     = p.i;
        pl.placement_id = p.p;
        pl.x_cell       = t->cur_col;
        pl.y_cell       = t->cur_row;
        pl.src_x        = p.x; pl.src_y = p.y;
        pl.src_w        = p.w; pl.src_h = p.h;
        pl.cols         = p.c; pl.rows  = p.r;
        pl.z_index      = p.z;
        pl.alt          = t->in_alt_screen;
        ts.placements.push_back(pl);
        // Advance cursor for a=p placement too
        const KittyImage &pimg = s_images[p.i];
        int rows_used = p.r ? p.r :
            (pimg.ph > 0 && t->cell_h > 0) ?
                (int)((pimg.ph + (int)t->cell_h - 1) / (int)t->cell_h) : 1;
        for (int r = 0; r < rows_used; r++)
            term_newline(t);
        t->cur_col = 0;
        send_response(t, p.i, p.q, true, nullptr);
    }
}

// ============================================================================
// kitty_tick — advance animation timers
// ============================================================================

bool kitty_tick(double dt) {
    bool changed = false;
    double dt_ms = dt * 1000.0;

    for (auto &kv : s_anim) {
        KittyAnimCtrl &ac = kv.second;
        if (ac.state == AnimState::STOPPED) continue;

        auto iit = s_images.find(kv.first);
        if (iit == s_images.end()) continue;
        KittyImage &img = iit->second;
        if (!img.animated || img.frames.size() < 2) continue;

        ac.accum_ms += dt_ms;

        // Duration for the current frame
        int dur = ac.gap_ms > 0 ? ac.gap_ms : img.frames[ac.cur_frame].duration_ms;
        if (dur <= 0) dur = 100;

        while (ac.accum_ms >= dur) {
            ac.accum_ms -= dur;
            int next = ac.cur_frame + 1;
            if (next >= (int)img.frames.size()) {
                next = 0;
                if (ac.state == AnimState::ONCE) {
                    ac.state = AnimState::STOPPED;
                    ac.cur_frame = (int)img.frames.size() - 1;
                    changed = true;
                    break;
                }
                if (ac.loops_left > 0) {
                    ac.loops_left--;
                    if (ac.loops_left == 0) {
                        ac.state = AnimState::STOPPED;
                        ac.cur_frame = (int)img.frames.size() - 1;
                        changed = true;
                        break;
                    }
                }
            }
            if (ac.cur_frame != next) {
                ac.cur_frame = next;
                changed = true;
            }
            // recalculate duration for new frame
            dur = ac.gap_ms > 0 ? ac.gap_ms : img.frames[ac.cur_frame].duration_ms;
            if (dur <= 0) dur = 100;
        }
    }
    return changed;
}

// ============================================================================
// kitty_render
// ============================================================================

void kitty_render(Terminal *t, int ox, int oy, int clip_rows) {
    int vis_rows = (clip_rows > 0 && clip_rows < t->rows) ? clip_rows : t->rows;
    auto tit = s_terms.find(t);
    if (tit == s_terms.end()) return;
    KittyTermState &ts = tit->second;
    if (ts.placements.empty()) return;

    std::stable_sort(ts.placements.begin(), ts.placements.end(),
        [](const KittyPlacement &a, const KittyPlacement &b){ return a.z_index < b.z_index; });

    if (g_use_sdl_renderer) {
        // SDL path: flush vertex geometry, then RenderCopyF each placement
        gl_flush_verts();
        for (const KittyPlacement &pl : ts.placements) {
            if (pl.alt != t->in_alt_screen) continue;   // other screen's image
            auto iit = s_images.find(pl.image_id);
            if (iit == s_images.end()) continue;
            const KittyImage &img = iit->second;

            SDL_Texture *tex = img.sdl_tex;
            int ipw = img.pw, iph = img.ph;
            if (img.animated && !img.frames.empty()) {
                auto ait = s_anim.find(pl.image_id);
                int fi = (ait != s_anim.end()) ? ait->second.cur_frame : 0;
                if (fi < 0) fi = 0;
                if (fi >= (int)img.frames.size()) fi = (int)img.frames.size() - 1;
                const KittyFrame &fr = img.frames[fi];
                if (fr.sdl_tex) { tex = fr.sdl_tex; ipw = fr.pw; iph = fr.ph; }
            }
            if (!tex) continue;

            float cw = t->cell_w, ch = t->cell_h;
            float disp_w = pl.cols ? pl.cols * cw : (float)ipw;
            float disp_h = pl.rows ? pl.rows * ch : (float)iph;
            float vis_row = (float)(pl.y_cell + t->sb_offset);

            if (vis_row + disp_h / ch <= 0 || vis_row >= (float)vis_rows) continue;

            float dx = ox + pl.x_cell * cw;
            float dy = oy + vis_row * ch;

            // Source rect (crop)
            SDL_Rect src;
            src.x = pl.src_x;
            src.y = pl.src_y;
            src.w = pl.src_w ? pl.src_w : ipw;
            src.h = pl.src_h ? pl.src_h : iph;

            // Clip to terminal viewport
            float top_limit    = (float)oy;
            float bottom_limit = oy + vis_rows * ch;

            if (dy < top_limit) {
                float clip = top_limit - dy;
                float frac = clip / disp_h;
                src.y += (int)(frac * src.h);
                src.h -= (int)(frac * src.h);
                disp_h -= clip;
                dy = top_limit;
            }
            if (dy + disp_h > bottom_limit) {
                float clip = (dy + disp_h) - bottom_limit;
                float frac = clip / disp_h;
                src.h -= (int)(frac * src.h);
                disp_h -= clip;
            }
            if (disp_h <= 0 || src.h <= 0) continue;

            SDL_FRect dst = { dx, dy, disp_w, disp_h };
            SDL_RenderCopyF(g_sdl_renderer, tex, &src, &dst);
        }
        return;
    }

    // GL path — original shader-based render
    gl_flush_verts();

    glUseProgram(s_img_prog);
    glUniformMatrix4fv(s_img_proj, 1, GL_FALSE, G.proj.m);
    glUniform1i(s_img_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_img_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_img_vbo);

    for (const KittyPlacement &pl : ts.placements) {
        if (pl.alt != t->in_alt_screen) continue;   // other screen's image
        auto iit = s_images.find(pl.image_id);
        if (iit == s_images.end()) continue;
        const KittyImage &img = iit->second;
        if (!img.tex) continue;

        GLuint tex = img.tex;
        int    ipw = img.pw, iph = img.ph;
        if (img.animated && !img.frames.empty()) {
            auto ait = s_anim.find(pl.image_id);
            int fi = (ait != s_anim.end()) ? ait->second.cur_frame : 0;
            if (fi < 0) fi = 0;
            if (fi >= (int)img.frames.size()) fi = (int)img.frames.size() - 1;
            const KittyFrame &fr = img.frames[fi];
            if (fr.tex) { tex = fr.tex; ipw = fr.pw; iph = fr.ph; }
        }
        if (!tex) continue;

        float cw = t->cell_w, ch = t->cell_h;
        float disp_w = pl.cols ? pl.cols * cw : (float)ipw;
        float disp_h = pl.rows ? pl.rows * ch : (float)iph;

        float vis_row = (float)(pl.y_cell + t->sb_offset);
        float img_rows = disp_h / ch;
        if (vis_row + img_rows <= 0 || vis_row >= (float)vis_rows) continue;

        float dx = ox + pl.x_cell * cw;
        float dy = oy + vis_row * ch;

        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
        if (ipw > 0 && iph > 0) {
            int sw = pl.src_w ? pl.src_w : ipw;
            int sh = pl.src_h ? pl.src_h : iph;
            u0 = (float)pl.src_x / ipw; v0 = (float)pl.src_y / iph;
            u1 = u0 + (float)sw / ipw;  v1 = v0 + (float)sh / iph;
        }

        if (dy < (float)oy) {
            float clip = (float)oy - dy;
            float frac = clip / disp_h;
            v0 += frac * (v1 - v0);
            dy  = (float)oy;
            disp_h -= clip;
        }
        float bottom_limit = oy + vis_rows * ch;
        if (dy + disp_h > bottom_limit) {
            float clip = (dy + disp_h) - bottom_limit;
            float frac = clip / disp_h;
            v1 -= frac * (v1 - v0);
            disp_h -= clip;
        }
        if (disp_h <= 0) continue;

        float verts[24] = {
            dx,        dy,        u0, v0,
            dx+disp_w, dy,        u1, v0,
            dx+disp_w, dy+disp_h, u1, v1,
            dx,        dy,        u0, v0,
            dx+disp_w, dy+disp_h, u1, v1,
            dx,        dy+disp_h, u0, v1,
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glUseProgram(0);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void kitty_init(void) {
    if (g_use_sdl_renderer) return;  // no GL resources needed in SDL path

    // Build shader
    auto compile = [](const char *src, GLenum type) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, NULL);
        glCompileShader(s);
        int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(s,512,NULL,log); SDL_Log("[Kitty] shader: %s\n",log); }
        return s;
    };
    GLuint vs = compile(IMG_VS, GL_VERTEX_SHADER);
    GLuint fs = compile(IMG_FS, GL_FRAGMENT_SHADER);
    s_img_prog = glCreateProgram();
    glAttachShader(s_img_prog, vs);
    glAttachShader(s_img_prog, fs);
    glLinkProgram(s_img_prog);
    glDeleteShader(vs); glDeleteShader(fs);

    s_img_proj = glGetUniformLocation(s_img_prog, "proj");
    s_img_tex  = glGetUniformLocation(s_img_prog, "img");

    // VAO / VBO for a single quad (updated every draw)
    glGenVertexArrays(1, &s_img_vao);
    glGenBuffers(1, &s_img_vbo);
    glBindVertexArray(s_img_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_img_vbo);
    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    // pos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void kitty_clear(Terminal *t) {
    auto it = s_terms.find(t);
    if (it != s_terms.end()) {
        it->second.placements.clear();
        it->second.has_pending = false;
    }
}

// ============================================================================
// Minimal PNG encoder — no external dependencies beyond zlib (already linked)
// Supports RGBA 8-bit only, which is all we need.
// ============================================================================
#include <zlib.h>

static bool encode_png(const uint8_t *rgba, int w, int h, int stride,
                       std::vector<uint8_t> &out) {
    // --- Filter each row (filter type 0 = None) and deflate ---
    // Raw image data: for each row, prepend filter byte 0x00
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int row = 0; row < h; row++) {
        raw.push_back(0x00);  // filter type: None
        raw.insert(raw.end(), rgba + row * stride, rgba + row * stride + w * 4);
    }

    // Deflate
    uLongf comp_bound = compressBound((uLong)raw.size());
    std::vector<uint8_t> compressed(comp_bound);
    uLongf comp_len = comp_bound;
    if (compress2(compressed.data(), &comp_len, raw.data(), (uLong)raw.size(), 6) != Z_OK)
        return false;
    compressed.resize(comp_len);

    // --- PNG helpers ---
    auto write_u32 = [&](uint32_t v) {
        out.push_back((v >> 24) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >>  8) & 0xFF);
        out.push_back((v      ) & 0xFF);
    };
    auto write_chunk = [&](const char type[4], const uint8_t *data, uint32_t len) {
        write_u32(len);
        uint32_t crc = (uint32_t)crc32(0, (const Bytef*)type, 4);
        if (len) crc = (uint32_t)crc32(crc, data, len);
        out.insert(out.end(), (const uint8_t*)type, (const uint8_t*)type + 4);
        if (len) out.insert(out.end(), data, data + len);
        write_u32(crc);
    };

    // PNG signature
    const uint8_t sig[] = {137,80,78,71,13,10,26,10};
    out.insert(out.end(), sig, sig + 8);

    // IHDR
    uint8_t ihdr[13] = {};
    ihdr[0]=(w>>24)&0xFF; ihdr[1]=(w>>16)&0xFF; ihdr[2]=(w>>8)&0xFF; ihdr[3]=w&0xFF;
    ihdr[4]=(h>>24)&0xFF; ihdr[5]=(h>>16)&0xFF; ihdr[6]=(h>>8)&0xFF; ihdr[7]=h&0xFF;
    ihdr[8]=8;   // bit depth
    ihdr[9]=6;   // colour type: RGBA
    ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    write_chunk("IHDR", ihdr, 13);

    // IDAT
    write_chunk("IDAT", compressed.data(), (uint32_t)compressed.size());

    // IEND
    write_chunk("IEND", nullptr, 0);

    return true;
}

bool kitty_encode_png(const uint8_t *rgba, int w, int h, int stride, std::vector<uint8_t> &out) {
    return encode_png(rgba, w, h, stride, out);
}

// ============================================================================
// BASE64 ENCODE
// ============================================================================

static const char B64CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const uint8_t *src, int len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (int i = 0; i < len; i += 3) {
        uint32_t b  = (uint32_t)src[i] << 16;
        if (i+1 < len) b |= (uint32_t)src[i+1] << 8;
        if (i+2 < len) b |= (uint32_t)src[i+2];
        out += B64CHARS[(b >> 18) & 0x3F];
        out += B64CHARS[(b >> 12) & 0x3F];
        out += (i+1 < len) ? B64CHARS[(b >>  6) & 0x3F] : '=';
        out += (i+2 < len) ? B64CHARS[(b      ) & 0x3F] : '=';
    }
    return out;
}

// ============================================================================
// kitty_get_png_images / kitty_get_html_images
// ============================================================================

std::vector<KittyPngImage> kitty_get_png_images(Terminal *t, int row_start, int row_end) {
    std::vector<KittyPngImage> result;

    auto tit = s_terms.find(t);
    if (tit == s_terms.end()) return result;

    for (const KittyPlacement &pl : tit->second.placements) {
        if (pl.alt != t->in_alt_screen) continue;   // not on the visible screen
        // pl.y_cell is a live-screen row (0 = top of current screen).
        // Selection rows use virtual coordinates: 0..sb_count-1 = scrollback,
        // sb_count..sb_count+rows-1 = live screen.
        int vrow = t->sb_count + pl.y_cell;
        if (vrow < row_start || vrow > row_end) continue;

        auto iit = s_images.find(pl.image_id);
        if (iit == s_images.end()) continue;
        const KittyImage &img = iit->second;
        if (!img.tex && !img.sdl_tex) continue;
        if (img.pw <= 0 || img.ph <= 0) continue;

        // Read pixels back from GPU
        std::vector<uint8_t> pixels((size_t)img.pw * img.ph * 4);
        if (g_use_sdl_renderer) {
            SDL_Texture *rt = SDL_CreateTexture(g_sdl_renderer,
                SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET,
                img.pw, img.ph);
            if (!rt) continue;
            SDL_Texture *prev_target = SDL_GetRenderTarget(g_sdl_renderer);
            SDL_SetRenderTarget(g_sdl_renderer, rt);
            SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 0);
            SDL_RenderClear(g_sdl_renderer);
            SDL_RenderCopy(g_sdl_renderer, img.sdl_tex, nullptr, nullptr);
            SDL_RenderReadPixels(g_sdl_renderer, nullptr,
                SDL_PIXELFORMAT_ABGR8888, pixels.data(), img.pw * 4);
            SDL_SetRenderTarget(g_sdl_renderer, prev_target);
            SDL_DestroyTexture(rt);
        } else {
            GLint prev_pack = 4;
            glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glBindTexture(GL_TEXTURE_2D, img.tex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
        }

        // Apply source crop if specified
        int sx = SDL_clamp(pl.src_x, 0, img.pw - 1);
        int sy = SDL_clamp(pl.src_y, 0, img.ph - 1);
        int sw = SDL_min(pl.src_w ? pl.src_w : img.pw, img.pw - sx);
        int sh = SDL_min(pl.src_h ? pl.src_h : img.ph, img.ph - sy);

        const uint8_t *src_ptr = pixels.data();
        std::vector<uint8_t> cropped;
        int stride = img.pw * 4;
        if (sx != 0 || sy != 0 || sw != img.pw || sh != img.ph) {
            cropped.resize((size_t)sw * sh * 4);
            for (int row = 0; row < sh; row++)
                memcpy(cropped.data() + (size_t)row * sw * 4,
                       pixels.data() + (size_t)(sy + row) * stride + sx * 4,
                       (size_t)sw * 4);
            src_ptr = cropped.data();
            stride  = sw * 4;
        }

        KittyPngImage entry;
        if (!encode_png(src_ptr, sw, sh, stride, entry.png)) continue;

        // Same sizing rules as kitty_render()
        float cw = t->cell_w, ch = t->cell_h;
        entry.vrow      = vrow;
        entry.cols      = pl.cols;
        entry.disp_w_px = pl.cols ? (int)(pl.cols * cw) : img.pw;
        entry.disp_h_px = pl.rows ? (int)(pl.rows * ch) : img.ph;
        entry.rows_used = pl.rows ? pl.rows :
            (ch > 0 ? (int)((entry.disp_h_px + (int)ch - 1) / (int)ch) : 1);
        if (entry.rows_used < 1) entry.rows_used = 1;
        result.push_back(std::move(entry));
    }

    std::stable_sort(result.begin(), result.end(),
        [](const KittyPngImage &a, const KittyPngImage &b){ return a.vrow < b.vrow; });
    return result;
}

std::vector<KittyHtmlImage> kitty_get_html_images(Terminal *t, int row_start, int row_end) {
    std::vector<KittyHtmlImage> result;
    std::vector<KittyPngImage> pngs = kitty_get_png_images(t, row_start, row_end);

    for (size_t i = 0; i < pngs.size(); i++) {
        const KittyPngImage &pi = pngs[i];
        int cols = pi.cols;

        // Build <img> tag with data URI, wrapped in a download-anchor so
        // "Save Image As" offers a sequential filename (image1.png, image2.png,
        // ...) instead of falling back to a generic default like "untitled.png".
        std::string data_uri = "data:image/png;base64," +
                               b64_encode(pi.png.data(), (int)pi.png.size());
        char fname[32];
        snprintf(fname, sizeof(fname), "image%d.png", (int)i + 1);

        std::string tag = "<a class=\"img-dl\" href=\"" + data_uri + "\" download=\"" + fname + "\">";
        tag += "<img src=\"" + data_uri + "\"";
        char wbuf[64];
        snprintf(wbuf, sizeof(wbuf), " style=\"width:%dch\"", cols);
        tag += (cols > 0) ? wbuf : " style=\"max-width:100%\"";
        tag += " alt=\"[terminal image]\">";
        tag += "</a>";

        KittyHtmlImage entry;
        entry.y_cell  = pi.vrow;
        entry.cols    = cols;
        entry.img_tag = std::move(tag);
        result.push_back(std::move(entry));
    }
    return result;
}

// ED 2 (clear screen): drop placements on the visible part of the current
// screen, the same as their text. Placements already in the scrollback stay
// until ED 3 or until they fall off the buffer. Image data is kept, since
// other placements may still reference it (same as kitty_clear).
void kitty_erase_screen(Terminal *t) {
    auto it = s_terms.find(t);
    if (it == s_terms.end()) return;
    auto &pv = it->second.placements;
    bool alt = t->in_alt_screen;
    pv.erase(std::remove_if(pv.begin(), pv.end(),
        [&](const KittyPlacement &pl) {
            if (pl.alt != alt) return false;
            int h = pl.rows;
            if (!h) {
                auto iit = s_images.find(pl.image_id);
                int ph = (iit != s_images.end()) ? iit->second.ph : 0;
                h = (ph > 0 && t->cell_h > 0) ? (int)((ph + (int)t->cell_h - 1) / (int)t->cell_h) : 1;
            }
            return pl.y_cell + h > 0 && pl.y_cell < t->rows;
        }), pv.end());
}

void kitty_scroll(Terminal *t, int lines) {
    auto it = s_terms.find(t);
    if (it == s_terms.end()) return;
    auto &pv = it->second.placements;
    bool alt = t->in_alt_screen;
    // Only the screen being scrolled moves; the other screen's images stay
    // put (e.g. an image on the normal screen while vim scrolls).
    for (auto &pl : pv) if (pl.alt == alt) pl.y_cell -= lines;

    // Keep placements while any part of them is still reachable in the
    // scrollback, so scrolling back (Shift+PgUp / wheel) shows the image
    // alongside its text. y_cell goes negative once the image is above the
    // live screen; a placement is gone only when its bottom edge has fallen
    // off the top of the scrollback buffer too. The alternate screen has no
    // scrollback, so there it's dropped as soon as it leaves the screen.
    // (Called after sb_push(), so sb_count already includes the new line.)
    int limit = alt ? 0 : -t->sb_count;
    pv.erase(std::remove_if(pv.begin(), pv.end(),
        [&](const KittyPlacement &pl) {
            if (pl.alt != alt) return false;
            int h = pl.rows;
            if (!h) {
                auto iit = s_images.find(pl.image_id);
                int ph = (iit != s_images.end()) ? iit->second.ph : 0;
                h = (ph > 0 && t->cell_h > 0) ? (int)((ph + (int)t->cell_h - 1) / (int)t->cell_h) : 1;
            }
            return (pl.y_cell + h) <= limit;
        }), pv.end());
}

void kitty_leave_alt_screen(Terminal *t) {
    auto it = s_terms.find(t);
    if (it == s_terms.end()) return;
    auto &pv = it->second.placements;
    pv.erase(std::remove_if(pv.begin(), pv.end(),
        [](const KittyPlacement &pl) { return pl.alt; }), pv.end());
}

void kitty_shutdown(void) {
    for (auto &kv : s_images) {
        for (auto &fr : kv.second.frames)
            free_tex(fr.tex, fr.sdl_tex);
        free_tex(kv.second.tex, kv.second.sdl_tex);
    }
    s_images.clear();
    s_anim.clear();
    s_terms.clear();
    if (!g_use_sdl_renderer) {
        if (s_img_prog) { glDeleteProgram(s_img_prog); s_img_prog = 0; }
        if (s_img_vao)  { glDeleteVertexArrays(1, &s_img_vao); s_img_vao = 0; }
        if (s_img_vbo)  { glDeleteBuffers(1, &s_img_vbo); s_img_vbo = 0; }
    }
}
