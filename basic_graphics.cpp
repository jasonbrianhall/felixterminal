// basic_graphics.cpp — BASIC graphics protocol for felix terminal
//
// Protocol: ESC ] 6 6 6 ; <cmd> ; <args...> ST
//
// Commands:
//   circle  ; x ; y ; r ; color
//   line    ; x1 ; y1 ; x2 ; y2 ; color [; B | BF]
//   pset    ; x ; y ; color
//   paint   ; x ; y ; color [; border_color]
//   cls     [; color]
//   screen  ; mode
//   palette ; index ; r ; g ; b
//   play    ; mml_string
//   get     ; sprite_id ; x1 ; y1 ; x2 ; y2
//   put     ; sprite_id ; x ; y [; pset|xor]
//   batch   ; <cmd1>\n<cmd2>\n...

#include "basic_graphics.h"
#include "mml_player.h"
#include "gl_renderer.h"
#include "sdl_renderer.h"
#include "term_color.h"
#include "gl_terminal.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ============================================================================
// DISPLAY LIST — commands queued by handle_osc, executed by basic_render()
// ============================================================================

struct BgCmd {
    std::string              name;
    std::vector<std::string> args;
};

static std::vector<BgCmd> s_display_list;
bool s_dl_dirty = false;

// Persistent background — set by cls, redrawn every frame so the FBO
// stays filled even when the display list is empty.
static float s_bg_r = 0.f, s_bg_g = 0.f, s_bg_b = 0.f, s_bg_a = 0.f;

// ============================================================================
// SCREEN / COORDINATE MAPPING
// ============================================================================

static int s_scr_w = 640;
static int s_scr_h = 350;
static int s_win_w = 800;
static int s_win_h = 480;

static float basic_x(int x, int w)  { return (float)x * w / s_scr_w; }
static float basic_y(int y, int h)  { return (float)y * h / s_scr_h; }
static float basic_sx(int w2, int w) { return (float)w2 * w / s_scr_w; }
static float basic_sy(int h2, int h) { return (float)h2 * h / s_scr_h; }

// ============================================================================
// BASIC-LOCAL PALETTE (separate from terminal's g_palette16)
// Keeps screen/palette commands from stomping the terminal's text colors.
// ============================================================================

static float s_basic_palette[16][3];
bool  s_basic_palette_active = false;

// ============================================================================
// COLOR RESOLUTION
// ============================================================================

static void resolve_color(int c, float *r, float *g, float *b) {
    if (c < 0) c = 0;
    if (c <= 15) {
        const float (*pal)[3] = s_basic_palette_active ? s_basic_palette : g_palette16;
        *r = pal[c][0];
        *g = pal[c][1];
        *b = pal[c][2];
    } else {
        *r = ((c >> 16) & 0xFF) / 255.f;
        *g = ((c >>  8) & 0xFF) / 255.f;
        *b = ((c      ) & 0xFF) / 255.f;
    }
}

// ============================================================================
// SPRITE STORE
// ============================================================================

struct BasicSprite {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    GLuint       tex     = 0;
    SDL_Texture *sdl_tex = nullptr;
};

static std::unordered_map<int, BasicSprite> s_sprites;

static void sprite_free(BasicSprite &sp) {
    if (g_use_sdl_renderer) {
        if (sp.sdl_tex) { SDL_DestroyTexture(sp.sdl_tex); sp.sdl_tex = nullptr; }
    } else {
        if (sp.tex) { glDeleteTextures(1, &sp.tex); sp.tex = 0; }
    }
}

static void sprite_upload(BasicSprite &sp) {
    sprite_free(sp);
    if (g_use_sdl_renderer) {
        sp.sdl_tex = SDL_CreateTexture(g_sdl_renderer,
            SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, sp.w, sp.h);
        if (sp.sdl_tex) {
            SDL_UpdateTexture(sp.sdl_tex, nullptr, sp.rgba.data(), sp.w * 4);
            SDL_SetTextureBlendMode(sp.sdl_tex, SDL_BLENDMODE_BLEND);
        }
    } else {
        glGenTextures(1, &sp.tex);
        glBindTexture(GL_TEXTURE_2D, sp.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sp.w, sp.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, sp.rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

// ============================================================================
// SOFTWARE CANVAS (for GET readback)
// ============================================================================

static std::vector<uint8_t> s_canvas;

static void canvas_resize() {
    s_canvas.assign((size_t)s_scr_w * s_scr_h * 4, 0);
}

static void canvas_pset(int x, int y, int color) {
    if (x < 0 || y < 0 || x >= s_scr_w || y >= s_scr_h) return;
    float r, g, b; resolve_color(color, &r, &g, &b);
    uint8_t *p = s_canvas.data() + (y * s_scr_w + x) * 4;
    p[0] = (uint8_t)(r * 255); p[1] = (uint8_t)(g * 255);
    p[2] = (uint8_t)(b * 255); p[3] = 0xFF;
}

static int canvas_point(int x, int y) {
    if (x < 0 || y < 0 || x >= s_scr_w || y >= s_scr_h) return -1;
    const uint8_t *p = s_canvas.data() + (y * s_scr_w + x) * 4;
    return ((int)p[0] << 16) | ((int)p[1] << 8) | p[2];
}

struct PaintSpan { int x0, x1, y; };

// QuickBASIC PAINT is a border fill: every pixel connected to the seed that
// is NOT the border colour is painted, whatever colour it is now. Pixels that
// already have the fill colour (or any other non-border colour) must not act
// as a wall. Border omitted = the fill colour. Visited pixels are tracked
// separately so the fill colour never stops or loops the scan. Returns the
// painted spans so the hardware layer can draw exactly those.
static std::vector<PaintSpan> canvas_paint(int x, int y, int fill_color, int border_color) {
    std::vector<PaintSpan> spans;
    const int W = s_scr_w, H = s_scr_h;
    if (x < 0 || y < 0 || x >= W || y >= H) return spans;
    if (border_color < 0) border_color = fill_color;
    float br, bg, bb; resolve_color(border_color, &br, &bg, &bb);
    int border_packed = ((int)(br*255)<<16)|((int)(bg*255)<<8)|(int)(bb*255);
    if (canvas_point(x, y) == border_packed) return spans;

    std::vector<uint8_t> seen((size_t)W * H, 0);
    auto open = [&](int px, int py) {
        return !seen[(size_t)py * W + px] && canvas_point(px, py) != border_packed;
    };
    std::vector<std::pair<int,int>> stk;
    stk.push_back({x, y});
    while (!stk.empty()) {
        auto [sx, sy] = stk.back(); stk.pop_back();
        if (!open(sx, sy)) continue;
        int x0 = sx, x1 = sx;
        while (x0 > 0     && open(x0 - 1, sy)) x0--;
        while (x1 < W - 1 && open(x1 + 1, sy)) x1++;
        for (int i = x0; i <= x1; i++) {
            seen[(size_t)sy * W + i] = 1;
            canvas_pset(i, sy, fill_color);
        }
        spans.push_back({x0, x1, sy});
        for (int ny = sy - 1; ny <= sy + 1; ny += 2) {
            if (ny < 0 || ny >= H) continue;
            bool in_run = false;
            for (int i = x0; i <= x1; i++) {
                if (open(i, ny)) { if (!in_run) { stk.push_back({i, ny}); in_run = true; } }
                else in_run = false;
            }
        }
    }
    return spans;
}

// ============================================================================
// HARDWARE DRAW HELPERS
// ============================================================================

static void hw_pset(int x, int y, int color) {
    float r, g, b; resolve_color(color, &r, &g, &b);
    float pw = basic_sx(1, s_win_w); if (pw < 1.f) pw = 1.f;
    float ph = basic_sy(1, s_win_h); if (ph < 1.f) ph = 1.f;
    draw_rect(basic_x(x, s_win_w), basic_y(y, s_win_h), pw, ph, r, g, b, 1.f);
}

// Emit a 1.5px wide line as a screen-space quad (two triangles).
static void hw_line(int x1, int y1, int x2, int y2, int color) {
    float r, g, b; resolve_color(color, &r, &g, &b);
    float ax = basic_x(x1,s_win_w), ay = basic_y(y1,s_win_h);
    float bx = basic_x(x2,s_win_w), by = basic_y(y2,s_win_h);
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) { draw_rect(ax,ay,1.f,1.f,r,g,b,1.f); return; }
    // Perpendicular normal, half-width = 0.75px
    float nx = -dy/len * 0.75f, ny = dx/len * 0.75f;
    Vertex v[6] = {
        {ax+nx, ay+ny, r,g,b,1.f}, {ax-nx, ay-ny, r,g,b,1.f}, {bx+nx, by+ny, r,g,b,1.f},
        {ax-nx, ay-ny, r,g,b,1.f}, {bx-nx, by-ny, r,g,b,1.f}, {bx+nx, by+ny, r,g,b,1.f},
    };
    draw_verts(v, 6, GL_TRIANGLES);
}

static void hw_rect(int x1, int y1, int x2, int y2, int color, bool filled) {
    float r, g, b; resolve_color(color, &r, &g, &b);
    float px = basic_x(x1,s_win_w), py = basic_y(y1,s_win_h);
    float pw = basic_sx(x2-x1, s_win_w), ph = basic_sy(y2-y1, s_win_h);
    if (filled) {
        draw_rect(px, py, pw, ph, r, g, b, 1.f);
    } else {
        hw_line(x1,y1,x2,y1,color);
        hw_line(x2,y1,x2,y2,color);
        hw_line(x2,y2,x1,y2,color);
        hw_line(x1,y2,x1,y1,color);
    }
}

static void hw_circle(int cx, int cy, int radius, int color) {
    float r, g, b; resolve_color(color, &r, &g, &b);
    float pcx = basic_x(cx, s_win_w);
    float pcy = basic_y(cy, s_win_h);
    float px_radius = basic_sx(radius, s_win_w);
    float py_radius = basic_sy(radius, s_win_h);
    float avg_px_radius = (px_radius + py_radius) * 0.5f;
    // One step per screen pixel of circumference — no gaps
    int STEPS = SDL_max(64, (int)(2.f * (float)M_PI * avg_px_radius) + 1);
    const float half_w = 0.75f;  // line half-width in screen pixels
    // Emit each arc segment as a quad (6 verts / 2 triangles)
    float prev_x = pcx + cosf(0) * px_radius;
    float prev_y = pcy + sinf(0) * py_radius;
    for (int i = 1; i <= STEPS; i++) {
        float angle = (float)i / STEPS * 2.f * (float)M_PI;
        float cur_x = pcx + cosf(angle) * px_radius;
        float cur_y = pcy + sinf(angle) * py_radius;
        float dx = cur_x - prev_x, dy = cur_y - prev_y;
        float len = sqrtf(dx*dx + dy*dy);
        if (len > 0.001f) {
            float nx = -dy/len * half_w, ny = dx/len * half_w;
            Vertex v[6] = {
                {prev_x+nx, prev_y+ny, r,g,b,1.f},
                {prev_x-nx, prev_y-ny, r,g,b,1.f},
                {cur_x +nx, cur_y +ny, r,g,b,1.f},
                {prev_x-nx, prev_y-ny, r,g,b,1.f},
                {cur_x -nx, cur_y -ny, r,g,b,1.f},
                {cur_x +nx, cur_y +ny, r,g,b,1.f},
            };
            draw_verts(v, 6, GL_TRIANGLES);
        }
        prev_x = cur_x; prev_y = cur_y;
    }
    // Software canvas — Bresenham for PAINT/GET accuracy
    int x = 0, y = radius, d = 1 - radius;
    while (x <= y) {
        canvas_pset(cx+x,cy+y,color); canvas_pset(cx-x,cy+y,color);
        canvas_pset(cx+x,cy-y,color); canvas_pset(cx-x,cy-y,color);
        canvas_pset(cx+y,cy+x,color); canvas_pset(cx-y,cy+x,color);
        canvas_pset(cx+y,cy-x,color); canvas_pset(cx-y,cy-x,color);
        if (d < 0) d += 2*x+3; else { d += 2*(x-y)+5; y--; }
        x++;
    }
}

// ============================================================================
// SPRITE PUT (hardware)
// ============================================================================

static GLuint s_spr_prog = 0, s_spr_vao = 0, s_spr_vbo = 0;
static GLint  s_spr_proj = -1, s_spr_tex = -1;

static const char *SPR_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec2 tc;\n"
    "uniform mat4 proj;\n"
    "out vec2 vTC;\n"
    "void main(){ gl_Position=proj*vec4(pos,0,1); vTC=tc; }\n";

static const char *SPR_FS =
    "#version 330 core\n"
    "in vec2 vTC; out vec4 frag; uniform sampler2D img;\n"
    "void main(){ frag=texture(img,vTC); }\n";

static void spr_init_gl() {
    auto compile = [](GLenum t, const char *s) {
        GLuint sh = glCreateShader(t);
        glShaderSource(sh, 1, &s, nullptr);
        glCompileShader(sh);
        return sh;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, SPR_VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, SPR_FS);
    s_spr_prog = glCreateProgram();
    glAttachShader(s_spr_prog, vs); glAttachShader(s_spr_prog, fs);
    glLinkProgram(s_spr_prog);
    glDeleteShader(vs); glDeleteShader(fs);
    s_spr_proj = glGetUniformLocation(s_spr_prog, "proj");
    s_spr_tex  = glGetUniformLocation(s_spr_prog, "img");
    glGenVertexArrays(1, &s_spr_vao);
    glGenBuffers(1, &s_spr_vbo);
    glBindVertexArray(s_spr_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_spr_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);
}

static void hw_put_sprite(int id, int x, int y, bool xor_mode) {
    auto it = s_sprites.find(id);
    if (it == s_sprites.end()) return;
    BasicSprite &sp = it->second;
    float px = basic_x(x,s_win_w), py = basic_y(y,s_win_h);
    float pw = basic_sx(sp.w,s_win_w), ph = basic_sy(sp.h,s_win_h);

    if (g_use_sdl_renderer) {
        if (!sp.sdl_tex) return;
        SDL_Rect dst = { (int)px, (int)py, (int)pw, (int)ph };
        SDL_SetTextureBlendMode(sp.sdl_tex, xor_mode ? SDL_BLENDMODE_MOD : SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(g_sdl_renderer, sp.sdl_tex, nullptr, &dst);
        return;
    }

    if (!sp.tex) return;
    gl_flush_verts();
    if (!s_spr_prog) spr_init_gl();

    float vdata[24] = {
        px,    py,    0.f,1.f,
        px+pw, py,    1.f,1.f,
        px+pw, py+ph, 1.f,0.f,
        px,    py,    0.f,1.f,
        px+pw, py+ph, 1.f,0.f,
        px,    py+ph, 0.f,0.f,
    };
    glUseProgram(s_spr_prog);
    glUniformMatrix4fv(s_spr_proj, 1, GL_FALSE, G.proj.m);
    glUniform1i(s_spr_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sp.tex);
    if (xor_mode) { glEnable(GL_COLOR_LOGIC_OP); glLogicOp(GL_XOR); }
    glBindVertexArray(s_spr_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_spr_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vdata), vdata, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    if (xor_mode) glDisable(GL_COLOR_LOGIC_OP);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(G.prog);
}

// ============================================================================
// ARGUMENT PARSING
// ============================================================================

static std::vector<std::string> split_args(const char *s, int len) {
    std::vector<std::string> out;
    const char *p = s, *end = s + len;
    while (p <= end) {
        const char *q = p;
        while (q < end && *q != ';') q++;
        out.emplace_back(p, q);
        p = q + 1;
    }
    return out;
}

static int argi(const std::vector<std::string> &a, int idx, int def = 0) {
    if (idx < 0 || idx >= (int)a.size()) return def;
    return atoi(a[idx].c_str());
}

// ============================================================================
// SCREEN MODE PALETTE SETUP
// ============================================================================

// Number of usable colors per screen mode (for clamping color indices)
static int s_max_colors = 16;

static void setup_screen_palette(int mode) {
    // Authentic QB/EGA/CGA palettes.
    // ANSI terminal palette (PAL_DEFAULT) is wrong for BASIC — index 1 is red
    // in ANSI but blue in EGA. Always load the hardware-accurate palette.

    // Standard EGA 16-color palette (used by SCREEN 7, 8, 9, 12 etc.)
    static const float PAL_EGA[16][3] = {
        {0.000f, 0.000f, 0.000f},  //  0 black
        {0.000f, 0.000f, 0.667f},  //  1 blue
        {0.000f, 0.667f, 0.000f},  //  2 green
        {0.000f, 0.667f, 0.667f},  //  3 cyan
        {0.667f, 0.000f, 0.000f},  //  4 red
        {0.667f, 0.000f, 0.667f},  //  5 magenta
        {0.667f, 0.333f, 0.000f},  //  6 brown
        {0.667f, 0.667f, 0.667f},  //  7 light gray
        {0.333f, 0.333f, 0.333f},  //  8 dark gray
        {0.333f, 0.333f, 1.000f},  //  9 light blue
        {0.333f, 1.000f, 0.333f},  // 10 light green
        {0.333f, 1.000f, 1.000f},  // 11 light cyan
        {1.000f, 0.333f, 0.333f},  // 12 light red
        {1.000f, 0.333f, 1.000f},  // 13 light magenta
        {1.000f, 1.000f, 0.333f},  // 14 yellow
        {1.000f, 1.000f, 1.000f},  // 15 white
    };

    // CGA 4-color palette 1 (most common): black, cyan, magenta, white
    static const float PAL_CGA4[4][3] = {
        {0.000f, 0.000f, 0.000f},
        {0.333f, 1.000f, 1.000f},
        {1.000f, 0.333f, 1.000f},
        {1.000f, 1.000f, 1.000f},
    };

    // Activate BASIC's own palette so g_palette16 (terminal text colors) is untouched.
    s_basic_palette_active = true;

    auto load_ega = [&]() {
        for (int i = 0; i < 16; i++) {
            s_basic_palette[i][0] = PAL_EGA[i][0];
            s_basic_palette[i][1] = PAL_EGA[i][1];
            s_basic_palette[i][2] = PAL_EGA[i][2];
        }
    };

    switch (mode) {
    case 0:  // text mode — relinquish palette back to terminal
        s_basic_palette_active = false;
        break;
    case 1:  // CGA 4-color
        s_max_colors = 4;
        for (int i = 0; i < 4; i++) {
            s_basic_palette[i][0] = PAL_CGA4[i][0];
            s_basic_palette[i][1] = PAL_CGA4[i][1];
            s_basic_palette[i][2] = PAL_CGA4[i][2];
        }
        break;
    case 2:  // CGA 2-color: black + white
        s_max_colors = 2;
        s_basic_palette[0][0]=0.f; s_basic_palette[0][1]=0.f; s_basic_palette[0][2]=0.f;
        s_basic_palette[1][0]=1.f; s_basic_palette[1][1]=1.f; s_basic_palette[1][2]=1.f;
        break;
    case 3:  // Hercules 2-color: black + white
        s_max_colors = 2;
        s_basic_palette[0][0]=0.f; s_basic_palette[0][1]=0.f; s_basic_palette[0][2]=0.f;
        s_basic_palette[1][0]=1.f; s_basic_palette[1][1]=1.f; s_basic_palette[1][2]=1.f;
        break;
    case 5: case 6:  // CGA low-res, 16-color EGA palette
        s_max_colors = 16;
        load_ega();
        break;
    case 7: case 8:  // EGA 16-color
        s_max_colors = 16;
        load_ega();
        break;
    case 9:  // EGA 64-color — 16 active, standard EGA palette
        s_max_colors = 16;
        load_ega();
        break;
    case 10: // EGA monochrome: black + green phosphor
        s_max_colors = 2;
        s_basic_palette[0][0]=0.f; s_basic_palette[0][1]=0.f;  s_basic_palette[0][2]=0.f;
        s_basic_palette[1][0]=0.f; s_basic_palette[1][1]=0.8f; s_basic_palette[1][2]=0.f;
        break;
    case 11: // VGA 2-color: black + white
        s_max_colors = 2;
        s_basic_palette[0][0]=0.f; s_basic_palette[0][1]=0.f; s_basic_palette[0][2]=0.f;
        s_basic_palette[1][0]=1.f; s_basic_palette[1][1]=1.f; s_basic_palette[1][2]=1.f;
        break;
    case 12: // VGA 16-color
        s_max_colors = 16;
        load_ega();
        break;
    case 13: // VGA 256-color — EGA palette for 0-15, rest are RGB ramp
        s_max_colors = 256;
        load_ega();
        break;
    case 21: case 22: case 23: // SVGA with render effects — EGA palette
        s_max_colors = 256;
        load_ega();
        break;
    case 24: case 25: // Tandy — EGA palette
        s_max_colors = 16;
        load_ega();
        break;
    default:
        s_max_colors = 256;
        load_ega();
        break;
    }
}

// Clamp a color index to the valid range for the current screen mode
static int clamp_color(int c) {
    if (c < 0) return 0;
    if (c < 16 && c >= s_max_colors) return c % s_max_colors;
    return c;
}

// ============================================================================
// COMMAND EXECUTION — called from basic_render() inside the GL frame
// ============================================================================

static void execute_cmd(const std::vector<std::string> &a) {
    if (a.empty()) return;
    const std::string &cmd = a[0];

    if (cmd == "pset") {
        int x=argi(a,1), y=argi(a,2), c=argi(a,3);
        c=clamp_color(c); hw_pset(x,y,c); canvas_pset(x,y,c);

    } else if (cmd == "line") {
        int x1=argi(a,1),y1=argi(a,2),x2=argi(a,3),y2=argi(a,4),c=clamp_color(argi(a,5));
        bool box    = a.size()>6 && (a[6]=="B"||a[6]=="BF");
        bool filled = a.size()>6 && a[6]=="BF";
        if (box) {
            hw_rect(x1,y1,x2,y2,c,filled);
        } else {
            hw_line(x1,y1,x2,y2,c);
            int dx=abs(x2-x1),dy=abs(y2-y1),sx=x1<x2?1:-1,sy=y1<y2?1:-1,err=dx-dy;
            int cx=x1,cy=y1;
            while(true){
                canvas_pset(cx,cy,c);
                if(cx==x2&&cy==y2) break;
                int e2=2*err;
                if(e2>-dy){err-=dy;cx+=sx;}
                if(e2< dx){err+=dx;cy+=sy;}
            }
        }

    } else if (cmd == "circle") {
        { int cc=clamp_color(argi(a,4)); hw_circle(argi(a,1),argi(a,2),argi(a,3),cc); }

    } else if (cmd == "paint") {
        int x=argi(a,1),y=argi(a,2),c=clamp_color(argi(a,3)),bc=a.size()>4?clamp_color(argi(a,4)):-1;
        float r,g,b; resolve_color(c,&r,&g,&b);
        // Border fill on the software canvas, then draw exactly the
        // painted spans on the hardware layer.
        for (const PaintSpan &sp : canvas_paint(x,y,c,bc)) {
            float px = basic_x(sp.x0, s_win_w);
            float py = basic_y(sp.y,  s_win_h);
            float pw = basic_sx(sp.x1 - sp.x0 + 1, s_win_w);
            float ph = basic_sy(1, s_win_h) + 1.f;
            draw_rect(px, py, pw, ph, r, g, b, 1.f);
        }

    } else if (cmd == "cls") {
        int c=a.size()>1?clamp_color(argi(a,1)):0;
        float r,g,b; resolve_color(c,&r,&g,&b);
        float alpha = (c == 0) ? 0.f : 1.f;
        // Remember this as the persistent background for future frames.
        s_bg_r = r; s_bg_g = g; s_bg_b = b; s_bg_a = alpha;
        // Always wipe the basic FBO to fully transparent first.
        // This ensures the terminal text/prompt is never permanently hidden.
        gl_basic_end();
        gl_basic_clear(s_win_w, s_win_h);
        gl_basic_begin(s_win_w, s_win_h);
        // Only draw a solid background if color is non-zero.
        // cls;0 means "clear graphics, show terminal" — leaves FBO transparent.
        if (alpha > 0.f)
            draw_rect(0,0,(float)s_win_w,(float)s_win_h,r,g,b,alpha);
        uint8_t cr=(uint8_t)(r*255),cg=(uint8_t)(g*255),cb=(uint8_t)(b*255);
        uint8_t ca=(uint8_t)(alpha*255);
        for(int i=0;i<s_scr_w*s_scr_h;i++){
            s_canvas[i*4+0]=cr; s_canvas[i*4+1]=cg;
            s_canvas[i*4+2]=cb; s_canvas[i*4+3]=ca;
        }

    } else if (cmd == "screen") {
        int mode=argi(a,1,9);
        setup_screen_palette(mode);
        // Resolutions per QB screen mode table.
        // Render effects mapped to visually appropriate modes.
        switch(mode){
        case 0:                                                              break; // text mode, no change
        case 1:  s_scr_w=320; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // CGA 4-color
        case 2:  s_scr_w=640; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // CGA 2-color
        case 3:  s_scr_w=720; s_scr_h=348; g_render_mode=RENDER_MODE_NORMAL; break; // Hercules
        case 4:  s_scr_w=640; s_scr_h=400; g_render_mode=RENDER_MODE_NORMAL; break; // Olivetti
        case 5:  s_scr_w=160; s_scr_h=100; g_render_mode=RENDER_MODE_NORMAL; break; // CGA low
        case 6:  s_scr_w=160; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // CGA low
        case 7:  s_scr_w=320; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL;     break; // EGA 16-color
        case 8:  s_scr_w=640; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL;     break; // EGA 16-color
        case 9:  s_scr_w=640; s_scr_h=350; g_render_mode=RENDER_MODE_NORMAL; break; // EGA 64-color (Gorilla)
        case 10: s_scr_w=640; s_scr_h=350; g_render_mode=RENDER_MODE_NORMAL; break; // EGA mono
        case 11: s_scr_w=640; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // VGA 2-color
        case 12: s_scr_w=640; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // VGA 16-color
        case 13: s_scr_w=320; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // VGA 256-color
        case 14: s_scr_w=320; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // PCP 16-color
        case 15: s_scr_w=640; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // PCP 4-color
        case 16: s_scr_w=640; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // PGC 256-color
        case 17: s_scr_w=640; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // IBM 8514/A
        case 18: s_scr_w=640; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // JEGA
        case 19: s_scr_w=640; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // JEGA text
        case 20: s_scr_w=512; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // TIGA
        case 21: s_scr_w=640; s_scr_h=400; g_render_mode=RENDER_MODE_NORMAL; break; // SVGA 256-color
        case 22: s_scr_w=640; s_scr_h=480; g_render_mode=RENDER_MODE_NORMAL; break; // SVGA 256-color
        case 23: s_scr_w=800; s_scr_h=600; g_render_mode=RENDER_MODE_NORMAL; break; // SVGA 256-color
        case 24: s_scr_w=160; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // Tandy/PCjr 16-color
        case 25: s_scr_w=320; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // Tandy/PCjr 16-color
        case 26: s_scr_w=640; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // Tandy
        case 27: s_scr_w=640; s_scr_h=200; g_render_mode=RENDER_MODE_NORMAL; break; // Tandy ETGA
        case 28: s_scr_w=720; s_scr_h=350; g_render_mode=RENDER_MODE_NORMAL; break; // OGA
        default: break;
        }
        canvas_resize();

    } else if (cmd == "palette") {
        int idx=argi(a,1),r=argi(a,2),g=argi(a,3),b=argi(a,4);
        if(idx>=0&&idx<16){
            s_basic_palette[idx][0]=r/255.f;
            s_basic_palette[idx][1]=g/255.f;
            s_basic_palette[idx][2]=b/255.f;
        }

    } else if (cmd == "play") {
        // Dispatched immediately (audio, not a GL command)
        if (a.size()>1) {
            SDL_Log("[BASIC] PLAY: %s\n", a[1].c_str());
            mml_play_via_wopr(a[1].c_str());
        }

    } else if (cmd == "get") {
        int id=argi(a,1),x1=argi(a,2),y1=argi(a,3),x2=argi(a,4),y2=argi(a,5);
        if(x2<x1) std::swap(x1,x2);
        if(y2<y1) std::swap(y1,y2);
        x1=SDL_clamp(x1,0,s_scr_w-1); x2=SDL_clamp(x2,0,s_scr_w-1);
        y1=SDL_clamp(y1,0,s_scr_h-1); y2=SDL_clamp(y2,0,s_scr_h-1);
        int sw=x2-x1+1, sh=y2-y1+1;
        BasicSprite &sp = s_sprites[id];
        sprite_free(sp);
        sp.w=sw; sp.h=sh;
        sp.rgba.resize((size_t)sw*sh*4);
        for(int row=0;row<sh;row++)
            memcpy(sp.rgba.data()+row*sw*4,
                   s_canvas.data()+((y1+row)*s_scr_w+x1)*4, sw*4);
        sprite_upload(sp);

    } else if (cmd == "put") {
        int id=argi(a,1),x=argi(a,2),y=argi(a,3);
        bool xormode = a.size()>4 && a[4]=="xor";
        hw_put_sprite(id,x,y,xormode);
        auto it=s_sprites.find(id);
        if(it!=s_sprites.end()){
            BasicSprite &sp=it->second;
            for(int row=0;row<sp.h;row++){
                int dy=y+row; if(dy<0||dy>=s_scr_h) continue;
                for(int col=0;col<sp.w;col++){
                    int dx=x+col; if(dx<0||dx>=s_scr_w) continue;
                    const uint8_t *src=sp.rgba.data()+(row*sp.w+col)*4;
                    uint8_t *dst=s_canvas.data()+(dy*s_scr_w+dx)*4;
                    if(xormode){ dst[0]^=src[0]; dst[1]^=src[1]; dst[2]^=src[2]; dst[3]=0xFF; }
                    else        { memcpy(dst,src,4); }
                }
            }
        }
    }
}

// ============================================================================
// COMMAND QUEUE — called from handle_osc (any thread context)
// ============================================================================

static void queue_cmd(const std::vector<std::string> &a) {
    if (a.empty()) return;
    // play is audio-only, execute immediately without queuing for GL frame
    if (a[0] == "play") { execute_cmd(a); return; }
    // screen mode change also takes effect immediately (no GL draw)
    if (a[0] == "screen" || a[0] == "palette") { execute_cmd(a); return; }
    s_display_list.push_back({ a[0], a });
    s_dl_dirty = true;
}

// ============================================================================
// basic_render — call from gl_terminal_main inside gl_begin_term_frame()
// ============================================================================

void basic_render(int win_w, int win_h) {
    s_win_w = win_w;
    s_win_h = win_h;

    // The basic FBO is persistent — only redraw when there are new commands.
    if (s_display_list.empty()) return;

    gl_basic_begin(win_w, win_h);
    for (auto &c : s_display_list)
        execute_cmd(c.args);
    gl_basic_end();
    s_display_list.clear();
    s_dl_dirty = false;
}

// ============================================================================
// basic_handle_osc — entry point called from terminal.cpp
// ============================================================================

void basic_handle_osc(Terminal * /*t*/, const char *payload, int len,
                      int win_w, int win_h) {
    s_win_w = win_w;
    s_win_h = win_h;
    if (len <= 0) return;

    if (len >= 5 && strncmp(payload, "batch", 5) == 0 && payload[5] == ';') {
        const char *p = payload + 6;
        int rem = len - 6;
        while (rem > 0) {
            const char *nl = (const char*)memchr(p, '\n', rem);
            int line_len = nl ? (int)(nl-p) : rem;
            if (line_len > 0) queue_cmd(split_args(p, line_len));
            if (!nl) break;
            rem -= (int)(nl-p)+1;
            p = nl+1;
        }
    } else {
        queue_cmd(split_args(payload, len));
    }
}

// ============================================================================
// INIT / SHUTDOWN
// ============================================================================

void basic_graphics_init(int win_w, int win_h) {
    s_win_w = win_w;
    s_win_h = win_h;
    canvas_resize();
}

void basic_graphics_shutdown(void) {
    for (auto &kv : s_sprites) sprite_free(kv.second);
    s_sprites.clear();
    s_canvas.clear();
    s_display_list.clear();
    if (!g_use_sdl_renderer) {
        if (s_spr_prog) { glDeleteProgram(s_spr_prog);         s_spr_prog = 0; }
        if (s_spr_vao)  { glDeleteVertexArrays(1, &s_spr_vao); s_spr_vao  = 0; }
        if (s_spr_vbo)  { glDeleteBuffers(1, &s_spr_vbo);      s_spr_vbo  = 0; }
    }
}
