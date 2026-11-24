// wopr_war.cpp — Global Thermonuclear War (interactive simulation)
//
// Features:
//   - ASCII world map with animated missile arcs and scrambling jets
//   - Click trajectory rows to inspect target detail
//   - ABORT LAUNCH button during countdown
//   - Hover highlighting
//   - Alert klaxon, radar ping, detonation boom, countdown ticks, ending chord

#include "wopr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <string>

#include "wopr_render.h"

// ─── Audio ─────────────────────────────────────────────────────────────────

static SDL_AudioDeviceID s_war_audio = 0;
static int               s_war_sample_rate = 22050;

static void war_audio_open() {
    if (s_war_audio) return;
    SDL_AudioSpec want{};
    want.freq     = s_war_sample_rate;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    s_war_audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (s_war_audio) SDL_PauseAudioDevice(s_war_audio, 0);
}
static void war_audio_close() {
    if (s_war_audio) { SDL_CloseAudioDevice(s_war_audio); s_war_audio = 0; }
}
static void war_queue(const Sint16 *buf, int n) {
    if (s_war_audio) SDL_QueueAudio(s_war_audio, buf, (Uint32)(n * sizeof(Sint16)));
}
static void war_sound_klaxon() {
    war_audio_open();
    const int R = s_war_sample_rate, N = R * 1;
    static Sint16 buf[22050];
    for (int i = 0; i < N; i++) {
        float t = (float)i / R;
        float freq = (((int)(t / 0.25f)) % 2 == 0) ? 440.f : 880.f;
        float v = sinf(2.f * 3.14159f * freq * t) > 0.f ? 1.f : -1.f;
        float env = (fmodf(t, 0.25f) < 0.22f) ? 1.f : 0.f;
        buf[i] = (Sint16)(12000.f * env * v);
    }
    war_queue(buf, N);
}
static void war_sound_ping() {
    war_audio_open();
    const int R = s_war_sample_rate, N = (int)(R * 0.18f);
    static Sint16 buf[4096];
    int n = std::min(N, 4096);
    for (int i = 0; i < n; i++) {
        float t = (float)i / R;
        float v = sinf(2.f * 3.14159f * (800.f + 1200.f * (t / 0.18f)) * t) * expf(-t * 25.f);
        buf[i] = (Sint16)(9000.f * v);
    }
    war_queue(buf, n);
}
static void war_sound_boom() {
    war_audio_open();
    const int R = s_war_sample_rate, N = (int)(R * 0.5f);
    static Sint16 buf[11025];
    int n = std::min(N, 11025);
    for (int i = 0; i < n; i++) {
        float t = (float)i / R, env = expf(-t * 12.f);
        float v = (((rand() & 0xFFFF) / 32768.f - 1.f) * 0.5f + sinf(2.f * 3.14159f * 55.f * t) * 0.6f) * env;
        if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
        buf[i] = (Sint16)(22000.f * v);
    }
    war_queue(buf, n);
}
static void war_sound_tick(int countdown) {
    war_audio_open();
    const int R = s_war_sample_rate, N = (int)(R * 0.06f);
    static Sint16 buf[2048];
    int n = std::min(N, 2048);
    float freq = 600.f + (15 - countdown) * 80.f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / R;
        buf[i] = (Sint16)(16000.f * sinf(2.f * 3.14159f * freq * t) * expf(-t * 120.f));
    }
    war_queue(buf, n);
}
static void war_sound_ending() {
    war_audio_open();
    const int R = s_war_sample_rate, N = (int)(R * 3.0f);
    static Sint16 buf[66150];
    int n = std::min(N, 66150);
    float freqs[3] = { 220.f, 261.6f, 329.6f };
    for (int i = 0; i < n; i++) {
        float t = (float)i / R, env = sinf(3.14159f * t / 3.0f), v = 0.f;
        for (float f : freqs) v += sinf(2.f * 3.14159f * f * t);
        buf[i] = (Sint16)(10000.f * env * v / 3.f);
    }
    war_queue(buf, n);
}

// ─── World map (ASCII, 76 cols x 30 rows, Mercator-ish) ────────────────────
// Lat range: ~75N to ~60S  (top to bottom)
// Lon range: ~170W to ~190E (left to right)
// Each cell approx 4.7 lon x 4.5 lat degrees

static const char *MAP_ROWS[] = {
"                        .....___......                                      ",
"             .._________)_____________)......   ___..                       ",
"          __(_________________________)_______)(____).....                  ",
"        _(________________________________________)_____)..                  ",
"      _(__________________________________________)_______).                 ",
"    .(_____________________________________________)_______).               ",
"   .(___________________________________________________)___).             ",
"   (______________________________________________________)__).___         ",
"   (________________________________________________________)____)..       ",
"   .(_______________________________________________)___________)..        ",
"    (_______________________________________________)__________).          ",
"    .(_______________________________________________)_______).            ",
"     (._______________________________________________)_____)              ",
"      .(_______________________________________________)_).               ",
"       .(______________________________________________)..    .___         ",
"        .(______________________________________________)   .(___)         ",
"         .(___________________________________________)   .(_____)         ",
"          .(________________________________________)    .(______)         ",
"           .(_____________________________________)    .(_______)          ",
"            .(__________________________________)    .(_________)          ",
"             .(______________________________).    .(__________)           ",
"              .(____________________________).   .(__________)             ",
"               .(__________________________).  .(__________)               ",
"                .(________________________).  .(________)                  ",
"                 .(____________________)..  .(________)                    ",
"                  .(_______________)..    .(_______)                       ",
"                   .(__________)..      .(_______)                         ",
"                    .(_______)..      .(_______)                           ",
"                     .(___).        .(_______)                             ",
"                      .(.).        .(_______)                              ",
};
static const int MAP_ROWS_COUNT = 30;
static const int MAP_COLS       = 76;
static const float MAP_LAT_TOP  =  75.f;
static const float MAP_LAT_BOT  = -60.f;
static const float MAP_LON_LEFT  = -170.f;
static const float MAP_LON_RIGHT =  190.f;

static void latlon_to_map(float lat, float lon, float &mc, float &mr) {
    mc = (lon - MAP_LON_LEFT) / (MAP_LON_RIGHT - MAP_LON_LEFT) * MAP_COLS;
    mr = (MAP_LAT_TOP - lat) / (MAP_LAT_TOP - MAP_LAT_BOT) * MAP_ROWS_COUNT;
}
static void map_to_px(float mc, float mr,
                       float mpx, float mpy, float mcw, float mch,
                       float &px, float &py) {
    px = mpx + mc * mcw;
    py = mpy + mr * mch;
}

// ─── Geographic data ───────────────────────────────────────────────────────

struct GeoCity { const char *name; float lat, lon; };
static const GeoCity GEO_CITIES[] = {
    { "LAS VEGAS, NV",    36.2f, -115.1f },
    { "LOS ANGELES, CA",  34.0f, -118.2f },
    { "SAN FRANCISCO, CA",37.8f, -122.4f },
    { "SEATTLE, WA",      47.6f, -122.3f },
    { "PORTLAND, OR",     45.5f, -122.7f },
    { "DENVER, CO",       39.7f, -104.9f },
    { "CHICAGO, IL",      41.9f,  -87.6f },
    { "DETROIT, MI",      42.3f,  -83.0f },
    { "NEW YORK, NY",     40.7f,  -74.0f },
    { "BOSTON, MA",       42.4f,  -71.1f },
    { "WASHINGTON, DC",   38.9f,  -77.0f },
    { "MIAMI, FL",        25.8f,  -80.2f },
    { "DALLAS, TX",       32.8f,  -96.8f },
    { "HOUSTON, TX",      29.8f,  -95.4f },
    { "PHOENIX, AZ",      33.4f, -112.1f },
    { "MOSCOW",           55.8f,   37.6f },
    { "LENINGRAD",        59.9f,   30.3f },
    { "KIEV",             50.5f,   30.5f },
    { "LONDON",           51.5f,   -0.1f },
    { "PARIS",            48.9f,    2.3f },
    { "BERLIN",           52.5f,   13.4f },
    { "BEIJING",          39.9f,  116.4f },
    { "TOKYO",            35.7f,  139.7f },
    { "SEOUL",            37.6f,  127.0f },
};
static const int GEO_CITY_COUNT = (int)(sizeof(GEO_CITIES)/sizeof(GEO_CITIES[0]));

struct GeoSite { const char *name; float lat, lon; bool is_jet; };
static const GeoSite GEO_SITES[] = {
    { "MINOT AFB, ND",      48.4f, -101.4f, false },
    { "WARREN AFB, WY",     41.1f, -104.8f, false },
    { "MALMSTROM AFB, MT",  47.5f, -111.2f, false },
    { "WHITEMAN AFB, MO",   38.7f,  -93.5f, true  },
    { "BARKSDALE AFB, LA",  32.5f,  -93.7f, true  },
    { "PLESETSK, USSR",     62.9f,   40.6f, false },
    { "BAIKONUR, USSR",     45.9f,   63.3f, false },
    { "TYURATAM, USSR",     45.6f,   63.4f, false },
};
static const int GEO_SITE_COUNT = (int)(sizeof(GEO_SITES)/sizeof(GEO_SITES[0]));

static bool city_geo(const char *name, float &lat, float &lon) {
    for (int i = 0; i < GEO_CITY_COUNT; i++)
        if (strcmp(GEO_CITIES[i].name, name) == 0) {
            lat = GEO_CITIES[i].lat; lon = GEO_CITIES[i].lon; return true;
        }
    lat = 40.f; lon = -100.f; return false;
}
static bool site_geo(const char *name, float &lat, float &lon, bool &is_jet) {
    for (int i = 0; i < GEO_SITE_COUNT; i++)
        if (strcmp(GEO_SITES[i].name, name) == 0) {
            lat = GEO_SITES[i].lat; lon = GEO_SITES[i].lon;
            is_jet = GEO_SITES[i].is_jet; return true;
        }
    lat = 45.f; lon = 63.f; is_jet = false; return false;
}

// ─── Sim data ──────────────────────────────────────────────────────────────

static const char *CITY_TARGETS[] = {
    "LAS VEGAS, NV",    "LOS ANGELES, CA",  "SAN FRANCISCO, CA",
    "SEATTLE, WA",      "PORTLAND, OR",     "DENVER, CO",
    "CHICAGO, IL",      "DETROIT, MI",      "NEW YORK, NY",
    "BOSTON, MA",       "WASHINGTON, DC",   "MIAMI, FL",
    "DALLAS, TX",       "HOUSTON, TX",      "PHOENIX, AZ",
    "MOSCOW",           "LENINGRAD",        "KIEV",
    "LONDON",           "PARIS",            "BERLIN",
    "BEIJING",          "TOKYO",            "SEOUL",
};
static const int CITY_COUNT = (int)(sizeof(CITY_TARGETS)/sizeof(CITY_TARGETS[0]));

static const char *LAUNCH_SITES[] = {
    "MINOT AFB, ND", "WARREN AFB, WY", "MALMSTROM AFB, MT",
    "WHITEMAN AFB, MO", "BARKSDALE AFB, LA",
    "PLESETSK, USSR", "BAIKONUR, USSR", "TYURATAM, USSR",
};
static const int SITE_COUNT = (int)(sizeof(LAUNCH_SITES)/sizeof(LAUNCH_SITES[0]));

static const char *WARHEAD_TYPES[] = {
    "W78  335 KT", "W87  300 KT", "W88  475 KT",
    "W62  170 KT", "SS-18 MOD4  750 KT", "SS-19 MOD3  500 KT",
};
static const int WARHEAD_COUNT = (int)(sizeof(WARHEAD_TYPES)/sizeof(WARHEAD_TYPES[0]));

static const char *STATUS_MSGS[] = {
    "LAUNCH DETECTED",        "TRAJECTORY CONFIRMED",
    "IMPACT ETA COMPUTING",   "COUNTERMEASURES NOMINAL",
    "EARLY WARNING ALERT",    "DEFCON LEVEL UPGRADED",
    "SAC FORCES ON ALERT",    "NORAD TRACKING ACTIVE",
};
static const int STATUS_COUNT = (int)(sizeof(STATUS_MSGS)/sizeof(STATUS_MSGS[0]));

struct WarTarget {
    std::string city, site, warhead;
    int   eta_sec;
    float progress;
    bool  detonated, is_jet;
    float src_lat, src_lon, dst_lat, dst_lon;
    float flash_t;
    float row_y, row_h;
};

struct WarState {
    double t, phase_t;
    int    phase;
    std::vector<WarTarget>   targets;
    std::vector<std::string> log_lines;
    int    countdown, last_countdown;
    bool   final_shown;
    double next_launch, next_status;
    int    hovered_target, selected_target;
    float  abort_x, abort_y, abort_w, abort_h;
    bool   abort_hovered, abort_pressed, abort_denied_shown;
    double abort_denied_t;
};

static WarState *ws(WoprState *w) { return (WarState *)w->sub_state; }

static void war_log(WarState *s, const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    s->log_lines.push_back(buf);
    if ((int)s->log_lines.size() > 20) s->log_lines.erase(s->log_lines.begin());
}

static void spawn_target(WarState *s) {
    WarTarget t;
    t.city    = CITY_TARGETS[rand() % CITY_COUNT];
    t.site    = LAUNCH_SITES[rand() % SITE_COUNT];
    t.warhead = WARHEAD_TYPES[rand() % WARHEAD_COUNT];
    t.eta_sec = 30 + rand() % 90;
    t.progress = 0.f; t.detonated = false; t.flash_t = 0.f;
    t.row_y = t.row_h = 0.f;
    site_geo(t.site.c_str(), t.src_lat, t.src_lon, t.is_jet);
    city_geo(t.city.c_str(), t.dst_lat, t.dst_lon);
    s->targets.push_back(t);
    war_sound_ping();
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────

void wopr_war_enter(WoprState *w) {
    war_audio_open();
    WarState *s = new WarState{};
    s->t = s->phase_t = 0; s->phase = 0;
    s->countdown = s->last_countdown = 60;
    s->final_shown = false;
    s->next_launch = 5.0; s->next_status = 4.0;
    s->hovered_target = s->selected_target = -1;
    s->abort_x = s->abort_y = s->abort_w = s->abort_h = 0.f;
    s->abort_hovered = s->abort_pressed = s->abort_denied_shown = false;
    s->abort_denied_t = 0.0;
    srand(0xDEFC0001);
    war_log(s, "WOPR STRATEGIC SIMULATION ENGAGED");
    war_log(s, "OPERATOR: JOSHUA / CLEARANCE: DELTA-5");
    war_log(s, "SCENARIO: GLOBAL THERMONUCLEAR WAR");
    war_log(s, ""); war_log(s, "INITIATING LAUNCH SEQUENCE...");
    w->sub_state = s;
}

void wopr_war_free(WoprState *w) {
    war_audio_close();
    delete ws(w);
    w->sub_state = nullptr;
}

// ─── Update ────────────────────────────────────────────────────────────────

void wopr_war_update(WoprState *w, double dt) {
    WarState *s = ws(w);
    if (!s || s->phase >= 4) return;
    s->t += dt; s->phase_t += dt;

    if (s->abort_denied_shown) {
        s->abort_denied_t += dt;
        if (s->abort_denied_t > 3.5) s->abort_denied_shown = false;
    }

    if (s->phase == 0 && s->phase_t >= 3.0) {
        s->phase = 1; s->phase_t = 0;
        for (int i = 0; i < 6; i++) spawn_target(s);
        war_log(s, ""); war_log(s, "LAUNCH DETECTION: 6 ICBMs INBOUND");
        war_sound_klaxon();
    }

    if (s->phase == 1) {
        for (int ti = 0; ti < (int)s->targets.size(); ti++) {
            auto &t = s->targets[ti];
            if (!t.detonated) {
                t.progress += (float)(dt / t.eta_sec);
                if (t.progress >= 1.f) {
                    t.progress = 1.f; t.detonated = true; t.flash_t = 0.6f;
                    war_log(s, "IMPACT: %-22s  [DETONATED]", t.city.c_str());
                    war_sound_boom();
                    if (s->selected_target == ti) s->selected_target = -1;
                }
            } else if (t.flash_t > 0.f) {
                t.flash_t -= (float)dt;
            }
        }
        if (s->phase_t >= s->next_launch && (int)s->targets.size() < 18) {
            spawn_target(s);
            int idx = (int)s->targets.size()-1;
            war_log(s, "LAUNCH: %-20s -> %s",
                    s->targets[idx].site.c_str(), s->targets[idx].city.c_str());
            s->next_launch += 2.0 + (rand()%30)/10.0;
        }
        if (s->phase_t >= s->next_status) {
            war_log(s, "[NORAD] %s", STATUS_MSGS[rand() % STATUS_COUNT]);
            s->next_status += 3.0 + (rand()%20)/10.0;
        }
        if (s->phase_t >= 30.0) {
            s->phase = 2; s->phase_t = 0;
            s->countdown = s->last_countdown = 15;
            war_log(s, ""); war_log(s, "*** RETALIATION SEQUENCE ARMED ***");
            war_log(s, "AUTHORIZATION CODE: ZULU-GOLF-NINER-NINER");
            war_sound_klaxon();
        }
    }

    if (s->phase == 2) {
        if (!s->abort_pressed) {
            int new_cd = 15 - (int)s->phase_t;
            if (new_cd != s->last_countdown && new_cd >= 0) {
                s->last_countdown = new_cd; s->countdown = new_cd;
                war_sound_tick(new_cd);
                if (new_cd % 5 == 0 || new_cd <= 5)
                    war_log(s, "  LAUNCH IN T-%02d SECONDS", new_cd);
            }
        }
        if (s->phase_t >= 16.0) { s->phase = 3; s->phase_t = 0; }
    }

    if (s->phase == 3 && !s->final_shown) {
        s->final_shown = true;
        for (auto &t : s->targets) { t.detonated = true; t.flash_t = 0.f; }
        war_log(s, ""); war_log(s, "GREETINGS, PROFESSOR FALKEN.");
        war_log(s, ""); war_log(s, "A STRANGE GAME.");
        war_log(s, "THE ONLY WINNING MOVE IS NOT TO PLAY.");
        war_log(s, ""); war_log(s, "HOW ABOUT A NICE GAME OF CHESS?");
        war_sound_ending();
        wopr_say("GREETINGS, PROFESSOR FALKEN.  A STRANGE GAME.  "
                 "THE ONLY WINNING MOVE IS NOT TO PLAY.  HOW ABOUT A NICE GAME OF CHESS?");
    }
    if (s->phase == 3 && s->phase_t >= 8.0) s->phase = 4;
}

// ─── Map arc drawing ───────────────────────────────────────────────────────

static void draw_arc(float sx, float sy, float dx, float dy,
                     float progress, bool is_jet,
                     float r, float g, float b,
                     float dot_w, float dot_h) {
    // Quadratic bezier: control point pulled north for ballistic arc
    float cx = (sx + dx) * 0.5f;
    float cy = (sy + dy) * 0.5f - fabsf(dx - sx) * (is_jet ? 0.12f : 0.38f)
                                 - fabsf(dy - sy) * (is_jet ? 0.05f : 0.22f);

    const int STEPS = 52;
    float prev_x = -9999.f, prev_y = -9999.f;
    for (int i = 0; i <= (int)(STEPS * progress); i++) {
        float t  = (float)i / STEPS;
        float mt = 1.f - t;
        float px = mt*mt*sx + 2*mt*t*cx + t*t*dx;
        float py = mt*mt*sy + 2*mt*t*cy + t*t*dy;
        if (fabsf(px-prev_x)+fabsf(py-prev_y) >= 3.5f) {
            float alpha = 0.2f + 0.8f * (t / std::max(progress, 0.01f));
            gl_draw_rect(px, py, dot_w, dot_h, r, g, b, alpha);
            prev_x = px; prev_y = py;
        }
    }

    // Leading tip glyph
    if (progress > 0.02f) {
        float t2 = progress, mt2 = 1.f - t2;
        float hx = mt2*mt2*sx + 2*mt2*t2*cx + t2*t2*dx;
        float hy = mt2*mt2*sy + 2*mt2*t2*cy + t2*t2*dy;
        float t1 = std::max(0.f, t2-0.025f), mt1 = 1.f-t1;
        float px1 = mt1*mt1*sx + 2*mt1*t1*cx + t1*t1*dx;
        float py1 = mt1*mt1*sy + 2*mt1*t1*cy + t1*t1*dy;
        float ddx = hx-px1, ddy = hy-py1;
        const char *glyph;
        if (!is_jet) glyph = fabsf(ddx)>fabsf(ddy) ? (ddx>0?">"  :"<" ) : (ddy>0?"v":"^");
        else         glyph = fabsf(ddx)>fabsf(ddy) ? (ddx>0?"}"  :"{"  ) : (ddy>0?"V":"^");
        gl_draw_text(glyph, hx-dot_w, hy-dot_h*0.5f, 1.f, is_jet?0.8f:1.f, 0.f, 1.f, 1.f);
    }
}

// ─── Render ────────────────────────────────────────────────────────────────

void wopr_war_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WarState *s = ws(w);
    if (!s) return;

    float scale = 1.f;
    float fch = (float)ch, fcw = (float)cw;
    float x0 = (float)ox, y0 = (float)oy;
    float total_w = (float)(cols * cw);

    // Title bar
    gl_draw_rect(x0-4, y0, total_w+8, fch+4, 0.f,0.4f,0.1f,0.5f);
    gl_draw_text("WOPR STRATEGIC WARFARE SIMULATION  --  CLASSIFIED  TOP SECRET",
                 x0, y0, 0.f,1.f,0.6f,1.f,scale);
    y0 += fch * 1.8f;

    // Layout: 62% map, 38% right panel
    float map_w   = total_w * 0.62f;
    float panel_x = x0 + map_w + fcw * 1.5f;
    float panel_w = total_w - map_w - fcw * 2.f;
    float mcw     = map_w / MAP_COLS;
    float mch     = fch * 0.92f;
    float map_h   = mch * MAP_ROWS_COUNT;

    // ── World map ──────────────────────────────────────────────────────────
    for (int row = 0; row < MAP_ROWS_COUNT; row++)
        gl_draw_text(MAP_ROWS[row], x0, y0 + row * mch,
                     0.f, 0.22f, 0.08f, 1.f, scale);

    // City dots
    for (int i = 0; i < GEO_CITY_COUNT; i++) {
        float mc, mr, px, py;
        latlon_to_map(GEO_CITIES[i].lat, GEO_CITIES[i].lon, mc, mr);
        map_to_px(mc, mr, x0, y0, mcw, mch, px, py);
        if (px >= x0 && px < x0 + map_w)
            gl_draw_text("+", px-fcw*0.5f, py-fch*0.5f, 0.f,0.55f,0.2f,0.9f,scale);
    }

    // Launch site icons
    for (int i = 0; i < GEO_SITE_COUNT; i++) {
        float mc, mr, px, py;
        latlon_to_map(GEO_SITES[i].lat, GEO_SITES[i].lon, mc, mr);
        map_to_px(mc, mr, x0, y0, mcw, mch, px, py);
        if (px >= x0 && px < x0 + map_w)
            gl_draw_text(GEO_SITES[i].is_jet?"A":"^",
                         px-fcw*0.5f, py-fch*0.5f, 0.f,0.7f,0.3f,0.85f,scale);
    }

    // Missile / jet arcs
    float dot_w = std::max(1.5f, mcw * 0.5f);
    float dot_h = std::max(1.5f, mch * 0.4f);

    for (auto &t : s->targets) {
        if (t.progress <= 0.f) continue;
        float smc, smr, dmc, dmr, spx, spy, dpx, dpy;
        latlon_to_map(t.src_lat, t.src_lon, smc, smr);
        latlon_to_map(t.dst_lat, t.dst_lon, dmc, dmr);
        map_to_px(smc, smr, x0, y0, mcw, mch, spx, spy);
        map_to_px(dmc, dmr, x0, y0, mcw, mch, dpx, dpy);

        if (t.detonated) {
            if (t.flash_t > 0.f && (SDL_GetTicks()/80)%2==0) {
                float sz = (0.6f - t.flash_t) * 32.f;
                gl_draw_rect(dpx-sz*0.5f, dpy-sz*0.5f, sz, sz, 1.f,0.6f,0.f,0.85f);
            }
            gl_draw_text(t.flash_t>0.f?"*":"X",
                         dpx-fcw*0.5f, dpy-fch*0.5f,
                         t.flash_t>0.f?1.f:0.5f,
                         t.flash_t>0.f?0.5f:0.1f,
                         0.f, t.flash_t>0.f?1.f:0.7f, scale);
        } else {
            float ar = t.is_jet?0.2f:1.f, ag = t.is_jet?0.9f:0.5f, ab = t.is_jet?0.8f:0.f;
            draw_arc(spx,spy,dpx,dpy, std::min(t.progress,1.f), t.is_jet,
                     ar,ag,ab, dot_w,dot_h);
        }
    }

    // Map legend
    gl_draw_text("^=SILO  A=AIRBASE  +=CITY  >=MISSILE  }=JET",
                 x0, y0+map_h+fch*0.3f, 0.f,0.3f,0.1f,1.f,scale);

    // ── Right panel ────────────────────────────────────────────────────────
    float ry = y0;
    gl_draw_text("TRAJECTORIES [CLICK]", panel_x, ry, 0.f,0.8f,0.3f,1.f,scale);
    ry += fch * 1.3f;

    float row_h  = fch * 1.2f;
    float bar_pw = panel_w * 0.38f;
    int   shown  = 0;

    for (int ti = 0; ti < (int)s->targets.size() && shown < 10; ti++) {
        auto &t = s->targets[ti];
        float ty = ry + shown * row_h;
        t.row_y = ty; t.row_h = row_h;

        bool sel = (s->selected_target == ti), hov = (s->hovered_target == ti);
        if (sel)
            gl_draw_rect(panel_x-2,ty,panel_w+2,row_h-1, 0.f,0.22f,0.07f,0.7f);
        else if (hov && !t.detonated)
            gl_draw_rect(panel_x-2,ty,panel_w+2,row_h-1, 0.f,0.12f,0.04f,0.5f);

        gl_draw_rect(panel_x, ty+fch*0.2f, bar_pw, fch*0.55f, 0.f,0.15f,0.04f,0.8f);
        float fr = t.detonated?0.9f:0.f, fg = t.detonated?0.15f:(sel?1.f:0.8f);
        gl_draw_rect(panel_x, ty+fch*0.2f, bar_pw*t.progress, fch*0.55f, fr,fg,0.f,0.9f);

        char label[40]; snprintf(label,sizeof(label),"%.16s",t.city.c_str());
        float tr2=t.detonated?1.f:(sel?0.3f:0.f), tg2=t.detonated?0.2f:(sel?1.f:0.85f);
        gl_draw_text(label, panel_x+bar_pw+fcw, ty, tr2,tg2,0.f,1.f,scale);
        shown++;
    }
    ry += shown * row_h + fch * 0.5f;

    // Detail panel
    if (s->selected_target >= 0 && s->selected_target < (int)s->targets.size()) {
        const auto &t = s->targets[s->selected_target];
        gl_draw_rect(panel_x-2,ry,panel_w+2,fch*4.6f, 0.f,0.18f,0.06f,0.6f);
        gl_draw_text("-- DETAIL --", panel_x+fcw,ry+fch*0.2f, 0.f,1.f,0.4f,1.f,scale);
        char line[64];
        snprintf(line,sizeof(line),"FROM: %.18s",t.site.c_str());
        gl_draw_text(line,panel_x+fcw,ry+fch*1.1f, 0.f,0.7f,0.3f,1.f,scale);
        snprintf(line,sizeof(line),"TO  : %.18s",t.city.c_str());
        gl_draw_text(line,panel_x+fcw,ry+fch*2.0f, 0.f,0.7f,0.3f,1.f,scale);
        snprintf(line,sizeof(line),"WH  : %.18s",t.warhead.c_str());
        gl_draw_text(line,panel_x+fcw,ry+fch*2.9f, 0.f,0.7f,0.3f,1.f,scale);
        if (t.detonated)
            gl_draw_text("*** DETONATED ***",panel_x+fcw,ry+fch*3.8f, 1.f,0.2f,0.f,1.f,scale);
        else {
            int eta=(int)((1.f-t.progress)*t.eta_sec);
            snprintf(line,sizeof(line),"ETA : T-%03d SEC",eta);
            gl_draw_text(line,panel_x+fcw,ry+fch*3.8f, 0.f,0.8f,0.3f,1.f,scale);
        }
        ry += fch * 5.0f;
    }

    // NORAD log
    gl_draw_text("NORAD LOG", panel_x, ry, 0.f,0.7f,0.25f,1.f,scale);
    ry += fch * 1.2f;
    int log_rows  = std::max(4, (int)((y0+map_h-ry)/fch));
    int log_start = std::max(0,(int)s->log_lines.size()-log_rows);
    for (int i = log_start; i < (int)s->log_lines.size(); i++) {
        bool fin = s->log_lines[i].find("STRANGE GAME")!=std::string::npos
                || s->log_lines[i].find("WINNING MOVE")!=std::string::npos
                || s->log_lines[i].find("HOW ABOUT")   !=std::string::npos
                || s->log_lines[i].find("GREETINGS")   !=std::string::npos;
        gl_draw_text(s->log_lines[i].c_str(), panel_x, ry,
                     fin?0.4f:0.f, fin?1.f:0.65f, fin?0.6f:0.2f, 1.f,scale);
        ry += fch;
    }

    // Countdown + abort
    if (s->phase == 2) {
        float cy = y0 + map_h + fch * 1.5f;
        char cd[64]; snprintf(cd,sizeof(cd),"LAUNCH COUNTDOWN: T-%02d",s->countdown);
        if ((SDL_GetTicks()/300)%2==0) gl_draw_text(cd,x0,cy, 1.f,0.1f,0.1f,1.f,scale);

        float bx=x0+fcw*28.f, by=cy, bw=fcw*20.f, bh=fch*1.3f;
        s->abort_x=bx; s->abort_y=by; s->abort_w=bw; s->abort_h=bh;
        if (!s->abort_pressed) {
            gl_draw_rect(bx,by,bw,bh, s->abort_hovered?0.75f:0.45f,0.f,0.f,0.85f);
            if ((SDL_GetTicks()/400)%2==0||s->abort_hovered)
                gl_draw_text("[ ABORT LAUNCH ]",bx+fcw*1.5f,by+fch*0.15f, 1.f,1.f,1.f,1.f,scale);
        } else { s->abort_x=0; s->abort_w=0; }
    }

    if (s->abort_denied_shown && (SDL_GetTicks()/200)%2==0)
        gl_draw_text("ABORT DENIED -- JOSHUA OVERRIDE ACTIVE",
                     x0, y0+map_h+fch*3.0f, 1.f,0.5f,0.f,1.f,scale);

    float fy = y0+map_h+fch*(s->phase==2?4.8f:1.5f);
    if (s->phase>=3)
        gl_draw_text("ESC=RETURN TO MENU",x0,fy, 0.f,0.6f,0.2f,1.f,scale);
    else
        gl_draw_text("ESC=ABORT  |  CLICK TRAJECTORY TO INSPECT",
                     x0,fy, 0.f,0.35f,0.1f,1.f,scale);
}

// ─── Input ─────────────────────────────────────────────────────────────────

bool wopr_war_keydown(WoprState *w, SDL_Keycode sym) { (void)w;(void)sym; return true; }

void wopr_war_mousedown(WoprState *w, int x, int y, int button) {
    WarState *s = ws(w);
    if (!s || button != 1) return;

    if (s->abort_w>0.f && x>=s->abort_x && x<=s->abort_x+s->abort_w
                       && y>=s->abort_y && y<=s->abort_y+s->abort_h) {
        s->abort_pressed=s->abort_denied_shown=true; s->abort_denied_t=0.0;
        war_log(s,""); war_log(s,"ABORT REQUEST DENIED -- JOSHUA OVERRIDE ACTIVE");
        war_sound_klaxon(); return;
    }
    for (int ti=0; ti<(int)s->targets.size()&&ti<10; ti++) {
        auto &t=s->targets[ti];
        if (t.row_h>0.f && y>=t.row_y && y<=t.row_y+t.row_h) {
            s->selected_target=(s->selected_target==ti)?-1:ti;
            war_sound_ping(); return;
        }
    }
    s->selected_target=-1;
}

void wopr_war_mousemove(WoprState *w, int x, int y) {
    WarState *s = ws(w);
    if (!s) return;
    s->abort_hovered = (s->abort_w>0.f && x>=s->abort_x && x<=s->abort_x+s->abort_w
                                        && y>=s->abort_y && y<=s->abort_y+s->abort_h);
    s->hovered_target=-1;
    for (int ti=0; ti<(int)s->targets.size()&&ti<10; ti++) {
        auto &t=s->targets[ti];
        if (t.row_h>0.f && !t.detonated && y>=t.row_y && y<=t.row_y+t.row_h)
            { s->hovered_target=ti; break; }
    }
}
