#include "wopr.h"
#include "wopr_salute.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <cctype>
#include "wopr_render.h"
#include "../felix_settings.h"
#include <ctime>

// ============================================================================
// GLOBALS
// ============================================================================

WoprState g_wopr;

// Current terminal foreground color (default: phosphor green)
uint8_t g_term_r = 0, g_term_g = 255, g_term_b = 69;

// ============================================================================
// KEYSTROKE AUDIO
// ============================================================================

// A dedicated one-shot SDL audio device for short keystroke clicks.
// Keeps it decoupled from the modem-screech audio pipeline.
static SDL_AudioDeviceID s_key_audio_dev = 0;

static void key_audio_ensure_open() {
    if (s_key_audio_dev != 0) return;
    SDL_AudioSpec want{};
    want.freq     = 22050;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    want.callback = nullptr;
    s_key_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (s_key_audio_dev != 0)
        SDL_PauseAudioDevice(s_key_audio_dev, 0);
}

void wopr_audio_play_keystroke() {
    key_audio_ensure_open();
    if (s_key_audio_dev == 0) return;

    // Keyboard thump: short noise burst (attack transient) + low-freq body decay.
    // Two-layer approach:
    //   1. Noise transient: ~4ms of white noise for the click attack
    //   2. Body: low sine ~120-180 Hz that decays fast, gives the "thock" weight
    const int   RATE = 22050;
    const int   N    = (int)(RATE * 0.040);  // 40ms total — enough for body to decay
    const float BODY_FREQ = 130.f + (rand() % 60);  // 130-190 Hz thump
    const float VOL  = 18000.f;

    static Sint16 buf[2048];
    int n = std::min(N, 2048);

    for (int i = 0; i < n; i++) {
        float t   = (float)i / RATE;

        // Noise transient: loud at start, dies in ~4ms
        float noise_env = expf(-t * 800.f);
        float noise     = ((rand() & 0xFFFF) / 32768.f - 1.f) * noise_env;

        // Low thump body: dies in ~25ms
        float body_env  = expf(-t * 120.f);
        float body      = sinf(2.f * 3.14159f * BODY_FREQ * t) * body_env;

        float sample = VOL * (noise * 0.4f + body * 0.7f);
        // Soft clip
        if (sample >  32000.f) sample =  32000.f;
        if (sample < -32000.f) sample = -32000.f;
        buf[i] = (Sint16)sample;
    }
    SDL_QueueAudio(s_key_audio_dev, buf, (Uint32)(n * sizeof(Sint16)));
}

// ============================================================================
// CONSTANTS
// ============================================================================

struct GameEntry {
    const char *label;
    const char *keyword;
    WoprGame    id;
};

static const GameEntry GAMES[] = {
    { "CHESS",                    "CHESS",       WoprGame::CHESS      },
    { "CHECKERS",                 "CHECKERS",    WoprGame::CHECKERS   },
    { "FALKEN'S MAZE",            "MAZE",        WoprGame::FALKEN_MAZE},
    { "GLOBAL THERMONUCLEAR WAR", "WAR",         WoprGame::GLOBAL_WAR },
    { "TIC-TAC-TOE",              "TIC",         WoprGame::TIC_TAC_TOE},
    { "MINESWEEPER",              "MINESWEEPER", WoprGame::MINESWEEPER},
    { "ZORK",                     "ZORK",        WoprGame::ZORK       },
    { "BASIC",                    "BASIC",       WoprGame::BASIC      },
    { "WIZARD'S CASTLE",          "WIZARD",      WoprGame::WIZARD     },
    { "WILLY THE WORM",           "WILLY",       WoprGame::WILLY_WORM },
    { "STRATEGIC DEFENSE INIT",   "SDI",         WoprGame::SDI        },
};
static const int GAME_COUNT = 10;

static const char *VALID_USER = "FALKEN";
static const char *VALID_PASS = "JOSHUA";

static const int INTEL_COUNT = 6;
static int g_intel_idx = 0;

static const int WIRE_STORY_COUNT = 8;
static int  g_wire_story_idx  = 0;

// Drip queue: lines waiting to be released one at a time into w->lines
static std::vector<std::string> g_drip_queue;
static double                   g_drip_acc   = 0.0;
static double                   g_drip_delay = 0.18;

// ============================================================================
// HELPERS
// ============================================================================

static std::string to_upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = (char)toupper((unsigned char)c);
    return r;
}

static void push_line_quiet(WoprState *w, const std::string &line) {
    // Stamp current terminal color as a \x01 fgR fgG fgB bgR bgG bgB prefix.
    // bg is always 0,0,0 for WOPR UI lines.
    extern uint8_t g_term_r, g_term_g, g_term_b;
    if (!line.empty() && (unsigned char)line[0] != 0x01) {
        std::string colored;
        colored.resize(7 + line.size());
        colored[0] = '\x01';
        colored[1] = (char)g_term_r;
        colored[2] = (char)g_term_g;
        colored[3] = (char)g_term_b;
        colored[4] = 0;  /* bg R */
        colored[5] = 0;  /* bg G */
        colored[6] = 0;  /* bg B */
        colored.replace(7, line.size(), line);
        w->lines.push_back(colored);
    } else {
        w->lines.push_back(line);
    }

    // Cap history at 5000 lines — drop oldest
    const int MAX_LINES = 5000;
    if ((int)w->lines.size() > MAX_LINES)
        w->lines.erase(w->lines.begin(),
                       w->lines.begin() + ((int)w->lines.size() - MAX_LINES));

    // New content: if user is scrolled back, keep their view position stable.
    // The clamp in wopr_render will prevent overscrolling automatically.
}

// Every line WOPR prints is spoken with VOICE ALL, except the user's own
// input echoed back (the WOPR> command line, the typed username/password).
static bool is_echo(const std::string &line) {
    std::string t = line;
    if (!t.empty() && (unsigned char)t[0] == 0x01) t = t.size() > 7 ? t.substr(7) : "";
    return t.rfind("WOPR> ", 0) == 0 || t.rfind("USERNAME:", 0) == 0 || t.rfind("PASSWORD:", 0) == 0;
}

static void push_line(WoprState *w, const std::string &line) {
    push_line_quiet(w, line);
    if (!is_echo(line)) wopr_say_any(line.c_str());
}

// A key line: printed, and spoken unless VOICE OFF.
static void say_line(WoprState *w, const std::string &line) {
    push_line_quiet(w, line);
    wopr_say(line.c_str());
}

static void begin_crawl(WoprState *w, const std::string &text) {
    if (!is_echo(text)) wopr_say_any(text.c_str());
    w->crawl_target = text;
    w->crawl_pos    = 0;
    w->crawl_acc    = 0.0;
}

static bool crawl_done(const WoprState *w) {
    return w->crawl_pos >= (int)w->crawl_target.size();
}

static void crawl_commit(WoprState *w) {
    if (!w->crawl_target.empty())
        push_line_quiet(w, w->crawl_target);       // already spoken when the crawl began
    w->crawl_target.clear();
    w->crawl_pos = 0;
    w->crawl_acc = 0.0;
}

static void set_phase(WoprState *w, WoprPhase p) {
    w->phase       = p;
    w->phase_timer = 0.0;
    w->crawl_pos   = 0;
    w->crawl_acc   = 0.0;
}

// ============================================================================
// COMMAND SHELL
// ============================================================================

static std::vector<std::string> g_cmd_history;
static int                      g_history_cursor = -1;

static void enter_shell(WoprState *w) {
    push_line(w, "");
    push_line(w, "TYPE  HELP  FOR AVAILABLE COMMANDS.");
    push_line(w, "");
    w->input_buf.clear();
    g_history_cursor = -1;
    set_phase(w, WoprPhase::GAME_MENU);
}

static std::string normalize(const std::string &s) {
    std::string r;
    bool sp = true;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!sp) { r += ' '; sp = true; }
        } else {
            r += (char)toupper((unsigned char)c);
            sp = false;
        }
    }
    if (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

static void split_verb(const std::string &cmd, std::string &verb, std::string &rest) {
    auto pos = cmd.find(' ');
    if (pos == std::string::npos) {
        verb = cmd; rest = "";
    } else {
        verb = cmd.substr(0, pos);
        rest = cmd.substr(pos + 1);
    }
}

static WoprGame find_game(const std::string &token) {
    for (int i = 0; i < GAME_COUNT; i++) {
        std::string lbl = GAMES[i].label;
        std::string kw  = GAMES[i].keyword;
        if (token == kw || token == lbl ||
            (token.size() >= 3 && lbl.find(token) != std::string::npos))
            return GAMES[i].id;
    }
    return WoprGame::NONE;
}

static void launch_game(WoprState *w, WoprGame game) {
    push_line(w, "");
    switch (game) {
        case WoprGame::TIC_TAC_TOE:
            say_line(w, "INITIATING: TIC-TAC-TOE");
            set_phase(w, WoprPhase::PLAYING_TTT);
            wopr_ttt_enter(w);
            break;
        case WoprGame::CHESS:
            say_line(w, "INITIATING: CHESS");
            set_phase(w, WoprPhase::PLAYING_CHESS);
            wopr_chess_enter(w);
            break;
        case WoprGame::CHECKERS:
            say_line(w, "INITIATING: CHECKERS");
            set_phase(w, WoprPhase::PLAYING_CHECKERS);
            wopr_checkers_enter(w);
            break;
        case WoprGame::MINESWEEPER:
            say_line(w, "INITIATING: MINESWEEPER");
            set_phase(w, WoprPhase::PLAYING_MINES);
            wopr_mines_enter(w);
            break;
        case WoprGame::FALKEN_MAZE:
            say_line(w, "INITIATING: FALKEN'S MAZE");
            set_phase(w, WoprPhase::PLAYING_MAZE);
            wopr_maze_enter(w);
            break;
        case WoprGame::GLOBAL_WAR:
            say_line(w, "INITIATING: GLOBAL THERMONUCLEAR WAR");
            push_line(w, "");
            say_line(w, "  WARNING: THIS SIMULATION USES LIVE TARGETING DATA.");
            say_line(w, "  ESTIMATED CASUALTIES: 4.2 BILLION.");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_WAR);
            wopr_war_enter(w);
            break;
        case WoprGame::ZORK:
            say_line(w, "INITIATING: ZORK");
            push_line(w, "");
            push_line(w, "  LOADING GREAT UNDERGROUND EMPIRE...");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_ZORK);
            wopr_zork_enter(w);
            break;
        case WoprGame::BASIC:
            say_line(w, "INITIATING: BASIC");
            push_line(w, "");
            push_line(w, "  LOADING ANTIQUATED PROGRAMMING LANGUAGE...");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_BASIC);
            wopr_basic_enter(w);
            break;
        case WoprGame::WIZARD:
            say_line(w, "INITIATING: WIZARD'S CASTLE");
            push_line(w, "");
            push_line(w, "  LOADING ADVENTURE SIMULATION...");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_BASIC);
            wopr_wizard_enter(w);
            break;
        case WoprGame::WILLY_WORM:
            say_line(w, "INITIATING: WILLY THE WORM");
            push_line(w, "");
            push_line(w, "  LOADING INVERTEBRATE SIMULATION...");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_WILLY);
            wopr_willy_enter(w);
            break;

        case WoprGame::SDI:
            say_line(w, "INITIATING: STRATEGIC DEFENSE INITIATIVE");
            push_line(w, "");
            push_line(w, "  MISSILE DEFENSE NETWORK ONLINE...");
            push_line(w, "  RADAR SYSTEMS ACTIVATED");
            push_line(w, "  DEFENSIVE PLATFORMS ARMED");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_SDI);
            wopr_sdi_enter(w);
            break;

        default: break;
    }
}

static void do_command(WoprState *w, const std::string &raw) {
    std::string cmd = normalize(raw);
    if (cmd.empty()) { push_line(w, ""); return; }

    // A new command interrupts the reading of the last one's output
    if (wopr_voice_mode() == WoprVoice::ALL) wopr_speech_stop();

    // Echo with prompt
    push_line(w, std::string("WOPR> ") + cmd);
    push_line(w, "");

    // History
    g_cmd_history.push_back(cmd);
    if (g_cmd_history.size() > 32) g_cmd_history.erase(g_cmd_history.begin());
    g_history_cursor = -1;

    std::string verb, rest;
    split_verb(cmd, verb, rest);

    // ── HELP ──────────────────────────────────────────────────────────────
    if (verb == "HELP" || verb == "?") {
        push_line(w, "  AVAILABLE COMMANDS:");
        push_line(w, "");
        push_line(w, "  HELP                  DISPLAY THIS MESSAGE");
        push_line(w, "  LIST GAMES            SHOW AVAILABLE SIMULATIONS");
        push_line(w, "  PLAY <game>           LAUNCH A SIMULATION");
        push_line(w, "  INTELLIGENCE          DISPLAY CURRENT THREAT ASSESSMENT");
        push_line(w, "  WIRE                  INCOMING WIRE SERVICE FEED");
        push_line(w, "  SITREP                SITUATION REPORT -- ALL THEATERS");
        push_line(w, "  STATUS                SYSTEM AND DEFCON STATUS");
        push_line(w, "  WHOAMI                DISPLAY CURRENT USER RECORD");
        push_line(w, "  HISTORY               COMMAND HISTORY");
        push_line(w, "  CLEAR                 CLEAR TERMINAL BUFFER");
        push_line(w, "  COLOR <preset|R G B>  SET TERMINAL COLOR  (GREEN AMBER BLUE WHITE RED CYAN)");
        push_line(w, "  VOICE ON|ALL|OFF      SPEAK KEY LINES / EVERYTHING / NOTHING");
        push_line(w, "  LOGOUT / QUIT / EXIT  TERMINATE SESSION");
        push_line(w, "");
        return;
    }

    // ── LIST (GAMES) ──────────────────────────────────────────────────────
    if (verb == "LIST" || cmd == "GAMES" || cmd == "LIST GAMES" || cmd == "SHOW GAMES") {
        push_line(w, "  AVAILABLE SIMULATIONS:");
        push_line(w, "");
        for (int i = 0; i < GAME_COUNT; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "    %-32s  (PLAY %s)", GAMES[i].label, GAMES[i].keyword);
            push_line(w, buf);
        }
        push_line(w, "");
        return;
    }

    // ── PLAY ──────────────────────────────────────────────────────────────
    if (verb == "PLAY" || verb == "RUN" || verb == "START") {
        if (rest.empty()) {
            push_line(w, "  USAGE:  PLAY <game name>");
            push_line(w, "  TYPE  LIST GAMES  FOR AVAILABLE SIMULATIONS.");
            return;
        }
        WoprGame g = find_game(rest);
        if (g == WoprGame::NONE) {
            push_line(w, "  SIMULATION NOT FOUND: " + rest);
            push_line(w, "  TYPE  LIST GAMES  TO SEE AVAILABLE SIMULATIONS.");
            return;
        }
        launch_game(w, g);
        return; // phase has changed; don't print trailing blank
    }

    // ── INTELLIGENCE ──────────────────────────────────────────────────────
    if (verb == "INTELLIGENCE" || verb == "INTEL" || verb == "THREAT") {
        push_line(w, "  *** CLASSIFIED -- EYES ONLY -- LEVEL 5 ***");
        push_line(w, "");
        const char *report = INTEL_REPORTS[g_intel_idx % INTEL_COUNT];
        g_intel_idx++;
        std::string line;
        for (const char *p = report; ; p++) {
            if (*p == '\n' || *p == '\0') {
                push_line(w, "  " + line);
                line.clear();
                if (*p == '\0') break;
            } else {
                line += *p;
            }
        }
        push_line(w, "");
        push_line(w, "  END OF REPORT.  DESTROY AFTER READING.");
        push_line(w, "");
        return;
    }

    // ── STATUS ────────────────────────────────────────────────────────────
    if (verb == "STATUS") {
        push_line(w, "  WOPR STRATEGIC DEFENSE SYSTEM  REV 4.1.7");
        push_line(w, "  UPTIME     : 847 DAYS  14 HRS  03 MIN");
        push_line(w, "  CPU LOAD   : 12.4%");
        push_line(w, "  SIMULATIONS: 19,432 COMPLETED THIS CYCLE");
        push_line(w, "");
        push_line(w, "  DEFCON STATUS    : 3");
        push_line(w, "  TRIAD READINESS  : GREEN");
        push_line(w, "  ABM NETWORK      : NOMINAL");
        push_line(w, "  SATELLITE UPLINK : ACTIVE  (3 OF 4 BIRDS ONLINE)");
        push_line(w, "");
        return;
    }

    // ── WHOAMI ────────────────────────────────────────────────────────────
    if (verb == "WHOAMI" || verb == "WHO" || cmd == "WHO AM I") {
        push_line(w, "  USER     : FALKEN, STEPHEN W.  DR.");
        push_line(w, "  CLEARANCE: TS/SCI  (CODEWORD: CRYSTAL WIND)");
        push_line(w, "  LAST LOGIN: 1983-06-03  03:14:07 MDT");
        push_line(w, "  SESSIONS : 1,847");
        push_line(w, "  NOTE     : ACCOUNT FLAGGED BY NSA -- SEE CASE FILE 7734");
        push_line(w, "");
        return;
    }

    // ── HISTORY ───────────────────────────────────────────────────────────
    if (verb == "HISTORY") {
        if (g_cmd_history.size() <= 1) {
            push_line(w, "  NO HISTORY.");
        } else {
            int start = std::max(0, (int)g_cmd_history.size() - 21); // -1 because we just added this cmd
            for (int i = start; i < (int)g_cmd_history.size() - 1; i++) {
                char buf[128];
                snprintf(buf, sizeof(buf), "  %3d  %s", i + 1, g_cmd_history[i].c_str());
                push_line(w, buf);
            }
        }
        push_line(w, "");
        return;
    }

    // ── CLEAR ─────────────────────────────────────────────────────────────
    if (verb == "CLEAR" || verb == "CLS") {
        w->lines.clear();
        push_line(w, "");
        return;
    }

    // ── COLOR ─────────────────────────────────────────────────────────────
    // Usage: COLOR <preset>   or   COLOR <R> <G> <B>   (0-255)
    // Presets: GREEN (default), AMBER, BLUE, WHITE, RED, CYAN
    if (verb == "COLOR" || verb == "COLOUR") {
        // Parse rest into tokens
        std::vector<std::string> toks;
        {
            std::string tok;
            for (char c : rest) {
                if (c == ' ') { if (!tok.empty()) { toks.push_back(tok); tok.clear(); } }
                else tok += c;
            }
            if (!tok.empty()) toks.push_back(tok);
        }

        struct ColorPreset { const char *name; uint8_t r, g, b; };
        static const ColorPreset PRESETS[] = {
            { "GREEN",  0,   255,  69  },
            { "AMBER",  255, 176,  0   },
            { "BLUE",   64,  196,  255 },
            { "WHITE",  220, 220,  220 },
            { "RED",    255, 60,   60  },
            { "CYAN",   0,   240,  200 },
        };
        static const int PRESET_COUNT = 6;

        if (toks.empty()) {
            push_line(w, "  USAGE:  COLOR <preset> | COLOR <R> <G> <B>");
            push_line(w, "  PRESETS: GREEN  AMBER  BLUE  WHITE  RED  CYAN");
            push_line(w, "");
            return;
        }

        uint8_t nr = 0, ng = 255, nb = 69; // default green
        bool matched = false;

        // Try numeric R G B
        if (toks.size() == 3) {
            int rv = atoi(toks[0].c_str());
            int gv = atoi(toks[1].c_str());
            int bv = atoi(toks[2].c_str());
            if (rv >= 0 && rv <= 255 && gv >= 0 && gv <= 255 && bv >= 0 && bv <= 255) {
                nr = (uint8_t)rv; ng = (uint8_t)gv; nb = (uint8_t)bv;
                matched = true;
            }
        }

        // Try preset name
        if (!matched) {
            for (int i = 0; i < PRESET_COUNT; i++) {
                if (toks[0] == PRESETS[i].name) {
                    nr = PRESETS[i].r; ng = PRESETS[i].g; nb = PRESETS[i].b;
                    matched = true;
                    break;
                }
            }
        }

        if (!matched) {
            push_line(w, "  UNKNOWN COLOR: " + toks[0]);
            push_line(w, "  PRESETS: GREEN  AMBER  BLUE  WHITE  RED  CYAN");
            push_line(w, "  OR: COLOR <R 0-255> <G 0-255> <B 0-255>");
            push_line(w, "");
            return;
        }

        // Store chosen color as a 4-byte prefix sentinel in a special line
        // The renderer already handles lines beginning with \x01 as color overrides
        // per-line. We repaint all existing lines and set a global terminal color
        // by re-writing them with the new prefix. Simpler approach: store the
        // color in a pair of globals and apply in wopr_render.
        // We use a minimal approach: prefix ALL future lines via a wrapper.
        // Inject a color-change sentinel into the lines vector so the renderer
        // picks it up — same \x01 R G B scheme already used there.
        // We set a module-level "current terminal color" that push_line prepends.
        extern uint8_t g_term_r, g_term_g, g_term_b;
        g_term_r = nr; g_term_g = ng; g_term_b = nb;

        char ack[64];
        snprintf(ack, sizeof(ack), "  TERMINAL COLOR SET TO  %u %u %u", nr, ng, nb);
        push_line(w, ack);
        push_line(w, "");
        return;
    }

    // ── VOICE ─────────────────────────────────────────────────────────────
    if (verb == "VOICE" || verb == "SPEECH") {
        if (rest == "ON" || rest == "KEY" || rest == "KEY LINES") {
            wopr_set_voice_mode(WoprVoice::KEY_LINES);
            settings_save();
            say_line(w, "  VOICE ON.  KEY LINES WILL BE SPOKEN.");
        } else if (rest == "ALL") {
            wopr_set_voice_mode(WoprVoice::ALL);
            settings_save();
            push_line(w, "  VOICE ON.  ALL OUTPUT WILL BE SPOKEN.");
        } else if (rest == "OFF") {
            wopr_set_voice_mode(WoprVoice::OFF);
            settings_save();
            push_line(w, "  VOICE OFF.");
        } else if (rest.empty()) {
            WoprVoice m = wopr_voice_mode();
            push_line(w, m == WoprVoice::OFF ? "  VOICE: OFF"
                       : m == WoprVoice::ALL ? "  VOICE: ALL OUTPUT" : "  VOICE: KEY LINES");
            push_line(w, "  USAGE:  VOICE ON | VOICE ALL | VOICE OFF");
        } else {
            push_line(w, "  USAGE:  VOICE ON | VOICE ALL | VOICE OFF");
        }
        push_line(w, "");
        return;
    }

    // ── LOGOUT / QUIT / EXIT ──────────────────────────────────────────────
    if (verb == "LOGOUT" || verb == "QUIT" || verb == "EXIT" || verb == "BYE") {
        static const char *BYE = "SESSION TERMINATED.  GOODBYE, PROFESSOR FALKEN.";
        w->crawl_target = BYE;                     // shown for the moment before closing
        SDL_Delay(100);
        wopr_close();
        wopr_say(BYE);                             // plays on after the overlay closes
        return;
    }

    // ── WIRE ──────────────────────────────────────────────────────────────
    if (verb == "WIRE" || cmd == "WIRE FEED" || cmd == "NEWS") {
        push_line(w, "  ** MILNET WIRE SERVICE -- UNCLASSIFIED FEED **");
        push_line(w, "  RECEIVING...");
        push_line(w, "");
        const char **story = WIRE_STORIES[g_wire_story_idx % WIRE_STORY_COUNT];
        g_wire_story_idx++;
        for (int i = 0; story[i] != nullptr; i++) {
            g_drip_queue.push_back(std::string("  ") + story[i]);
        }
        g_drip_queue.push_back("");
        g_drip_queue.push_back("  -- END TRANSMISSION --");
        g_drip_queue.push_back("");
        g_drip_acc = 0.0;
        return;
    }

    // ── SITREP ────────────────────────────────────────────────────────────
    if (verb == "SITREP" || cmd == "SIT REP" || cmd == "SITUATION REPORT") {

        srand((unsigned)time(nullptr));
        SaluteReport current_salute = SALUTE_POOL[rand()%SALUTE_POOL_SIZE];
        push_line(w, "Source: ");
        push_line(w, current_salute.source);
        push_line(w, "");

        push_line(w, "Activity:");
        push_line(w, current_salute.activity);
        if (current_salute.activity2 != NULL) push_line(w, current_salute.activity2);
        push_line(w, "");

        push_line(w, "Location:");
        push_line(w, current_salute.location);
        if (current_salute.activity2 != NULL) push_line(w, current_salute.location2);
        push_line(w, "");

        push_line(w, "Uniform:");
        push_line(w, current_salute.uniform);
        if (current_salute.activity2 != NULL) push_line(w, current_salute.uniform2);
        push_line(w, "");

        push_line(w, "Time:");
        push_line(w, current_salute.time_dtg);
        push_line(w, "");

        push_line(w, "Equipment:");
        push_line(w, current_salute.equipment);
        if (current_salute.activity2 != NULL) push_line(w, current_salute.equipment2);
        push_line(w, "");
        return;
    }

    // ── UNKNOWN ───────────────────────────────────────────────────────────
    push_line(w, "  COMMAND NOT RECOGNIZED: " + verb);
    push_line(w, "  TYPE  HELP  FOR AVAILABLE COMMANDS.");
    push_line(w, "");
}

// ============================================================================
// OPEN / CLOSE
// ============================================================================

void wopr_open() {
    WoprState *w = &g_wopr;

    // Tear down any sub-game that's still running before wiping state.
    if (w->visible) {
        switch (w->phase) {
            case WoprPhase::PLAYING_TTT:   wopr_ttt_free(w);   break;
            case WoprPhase::PLAYING_CHESS: wopr_chess_free(w); break;
            case WoprPhase::PLAYING_CHECKERS: wopr_checkers_free(w); break;
            case WoprPhase::PLAYING_MINES: wopr_mines_free(w); break;
            case WoprPhase::PLAYING_MAZE:  wopr_maze_free(w);  break;
            case WoprPhase::PLAYING_WAR:   wopr_war_free(w);   break;
            case WoprPhase::PLAYING_ZORK:  wopr_zork_free(w);  break;
            case WoprPhase::PLAYING_BASIC: wopr_basic_free(w); break;
            case WoprPhase::PLAYING_WILLY: wopr_willy_free(w); break;
            default: break;
        }
    }

    // Reset WOPR state
    *w = WoprState{};
    w->visible = true;
    w->lines.reserve(128);
    g_cmd_history.clear();
    g_history_cursor = -1;
    g_intel_idx      = 0;
    g_wire_story_idx = 0;
    g_drip_queue.clear();
    g_drip_acc       = 0.0;
    g_term_r = 0; g_term_g = 255; g_term_b = 69; // phosphor green

    wopr_speech_stop();
    wopr_speech_preload();          // unpack the voice's dictionary while the modem screeches
    wopr_audio_play_screech();

    // -----------------------------
    //  MODEM DIAL-UP INTRO SEQUENCE
    // -----------------------------

    push_line(w, "INITIALIZING ACOUSTIC COUPLER...");
    push_line(w, "");

    // Generate a safe fictional number (555 prefix)
    int n1 = rand() % 10;
    int n2 = rand() % 10;
    int n3 = rand() % 10;

    std::string dial =
        "DIALING 555-0" +
        std::to_string(n1) +
        std::to_string(n2) +
        std::to_string(n3) +
        "...";

    // Give the crawl its own line
    push_line(w, "");
    begin_crawl(w, dial);

    // After the crawl finishes, your drip/crawl system will naturally
    // allow these lines to appear in order.
    push_line(w, dial);
    push_line(w, "");
    push_line(w, "CONNECTING...");
    push_line(w, "CARRIER DETECTED 1200 BAUD");
    push_line(w, "");

    int node = 100 + (rand() % 900);
    std::string node_line =
        "MILNET NODE " + std::to_string(node) +
        " -- WOPR AUTHENTICATION REQUIRED";

    push_line(w, node_line);
    push_line(w, "");

    set_phase(w, WoprPhase::LOGIN_PROMPT);
    begin_crawl(w, "USERNAME: ");
    // Seed first keystroke delay so typing starts ~0.3s after prompt appears
    w->auto_type_delay = 0.30;
}




void wopr_close() {
    WoprState *w = &g_wopr;
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_free(w);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_free(w); break;
        case WoprPhase::PLAYING_CHECKERS: wopr_checkers_free(w); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_free(w); break;
        case WoprPhase::PLAYING_MAZE:  wopr_maze_free(w);  break;
        case WoprPhase::PLAYING_WAR:   wopr_war_free(w);   break;
        case WoprPhase::PLAYING_ZORK:  wopr_zork_free(w);  break;
        case WoprPhase::PLAYING_BASIC: wopr_basic_free(w); break;
        case WoprPhase::PLAYING_WILLY: wopr_willy_free(w); break;
        default: break;
    }
    wopr_audio_stop();
    wopr_speech_stop();
    w->visible = false;
    w->phase   = WoprPhase::INACTIVE;
}

// ============================================================================
// UPDATE
// ============================================================================

void wopr_update(double dt) {
    WoprState *w = &g_wopr;
    if (!w->visible) return;

    w->phase_timer += dt;

    // Advance crawl
    if (!w->crawl_target.empty() && w->crawl_pos < (int)w->crawl_target.size()) {
        w->crawl_acc += dt * w->crawl_baud / 10.0;
        int advance = (int)w->crawl_acc;
        if (advance > 0) {
            w->crawl_acc -= advance;
            w->crawl_pos = std::min(w->crawl_pos + advance,
                                    (int)w->crawl_target.size());
        }
    }

    switch (w->phase) {

    case WoprPhase::LOGIN_PROMPT:
        if (crawl_done(w)) {
            set_phase(w, WoprPhase::LOGIN_INPUT);
            w->input_buf.clear();
            // Brief pause before auto-typing begins in the new field
            if (w->auto_login) {
                w->auto_type_acc   = 0.0;
                w->auto_type_delay = w->username_done ? 0.25 : 0.30;
                w->auto_submit_wait = 0.0;
            }
        }
        break;

    case WoprPhase::LOGIN_INPUT:
        if (w->auto_login) {
            // Determine which credential we're typing right now
            const char *target = w->username_done ? "JOSHUA" : "FALKEN";
            int typed = (int)w->input_buf.size();

            if (typed < (int)strlen(target)) {
                // Still have characters to type
                w->auto_type_acc += dt;
                if (w->auto_type_acc >= w->auto_type_delay) {
                    w->auto_type_acc = 0.0;
                    // Pick next inter-key delay: 70-160 ms, human-ish rhythm
                    w->auto_type_delay = 0.07 + (rand() % 90) * 0.001;

                    char ch_typed = target[typed];
                    w->input_buf += (char)toupper((unsigned char)ch_typed);
                    wopr_audio_play_keystroke();
                }
            } else {
                // All chars typed — wait a beat then "press Enter"
                w->auto_submit_wait += dt;
                double enter_delay = w->username_done ? 0.35 : 0.45; // slight pause before Enter
                if (w->auto_submit_wait >= enter_delay) {
                    w->auto_submit_wait = 0.0;
                    w->auto_type_acc    = 0.0;
                    w->auto_type_delay  = 0.0;
                    wopr_audio_play_keystroke();

                    if (!w->username_done) {
                        w->username_entered = to_upper(w->input_buf);
                        push_line(w, "USERNAME: " + w->username_entered);
                        w->username_done = true;
                        w->input_buf.clear();
                        begin_crawl(w, "PASSWORD: ");
                        set_phase(w, WoprPhase::LOGIN_PROMPT);
                    } else {
                        w->password_entered = to_upper(w->input_buf);
                        push_line(w, "PASSWORD: " + w->password_entered);
                        w->input_buf.clear();
                        push_line(w, "");
                        set_phase(w, WoprPhase::CONNECTING);
                        begin_crawl(w, "LOGON ACCEPTED -- CONNECTING TO WOPR MAINFRAME...");
                        w->auto_login = false; // done — hand control back to user
                    }
                }
            }
        }
        break;

    case WoprPhase::LOGIN_REJECTED:
        if (w->phase_timer > 2.0) {
            push_line(w, "");
            w->username_done = false;
            w->input_buf.clear();
            if (w->reject_count >= 6) {
                push_line(w, "ACCESS DENIED. CONNECTION TERMINATED.");
                set_phase(w, WoprPhase::FAREWELL);
                begin_crawl(w, "");
            } else {
                set_phase(w, WoprPhase::LOGIN_PROMPT);
                begin_crawl(w, "USERNAME: ");
            }
        }
        break;

    case WoprPhase::CONNECTING:
        if (crawl_done(w) && w->phase_timer > 1.5) {
            crawl_commit(w);
            push_line(w, "");
            push_line(w, "  NORAD CHEYENNE MOUNTAIN COMPLEX");
            push_line(w, "  WARGAMES SYSTEMS DIVISION");
            push_line(w, "  CLASSIFIED -- LEVEL 5 CLEARANCE");
            push_line(w, "");
            say_line(w, "  GREETINGS, PROFESSOR FALKEN.");
            push_line(w, "  LAST LOGIN: 1983-06-03  03:14:07 MDT");
            push_line(w, "");
            say_line(w, "  SHALL WE PLAY A GAME?");
            enter_shell(w);
        }
        break;

    // CMD_PROMPT is stored in GAME_MENU slot
    case WoprPhase::GAME_MENU:
        // Drain wire drip queue
        if (!g_drip_queue.empty()) {
            g_drip_acc += dt;
            while (g_drip_acc >= g_drip_delay && !g_drip_queue.empty()) {
                g_drip_acc -= g_drip_delay;
                push_line(w, g_drip_queue.front());
                g_drip_queue.erase(g_drip_queue.begin());
            }
        }
        break;

    case WoprPhase::PLAYING_TTT:   wopr_ttt_update(w, dt);   break;
    case WoprPhase::PLAYING_CHESS: wopr_chess_update(w, dt); break;
    case WoprPhase::PLAYING_CHECKERS: wopr_checkers_update(w, dt); break;
    case WoprPhase::PLAYING_MINES: wopr_mines_update(w, dt); break;
    case WoprPhase::PLAYING_MAZE:  wopr_maze_update(w, dt);  break;
    case WoprPhase::PLAYING_WAR:   wopr_war_update(w, dt);   break;
    case WoprPhase::PLAYING_ZORK:  wopr_zork_update(w, dt);  break;
    case WoprPhase::PLAYING_BASIC: wopr_basic_update(w, dt); break;
    case WoprPhase::PLAYING_WILLY: wopr_willy_update(w, dt); break;
    case WoprPhase::PLAYING_SDI:   wopr_sdi_update(w, dt);   break;

    case WoprPhase::FAREWELL:
        if (crawl_done(w) && w->phase_timer > 2.5)
            wopr_close();
        break;

    default: break;
    }
}

// ============================================================================
// RENDER
// ============================================================================

void wopr_render(int win_w, int win_h) {
    WoprState *w = &g_wopr;
    if (!w->visible) return;

    const int   PAD   = 24;
    const float SCALE = 1.0f;
    float ch = gl_text_height(SCALE);
    float cw = gl_text_width("M", SCALE);
    if (ch < 1.f) ch = 16.f;
    if (cw < 1.f) cw = 10.f;

    // Full-screen dark background
    gl_draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 1.0f);

    float x0     = PAD + 8;
    float y0     = PAD + 8;
    float area_w = win_w - 2*(PAD + 8);
    int   vis_rows = (int)((win_h - 2*(PAD + 8)) / ch) - 2;

    // Sub-game rendering (games with their own full UI)
    // Zork is intentionally NOT here — it uses the terminal scroll path below
    bool in_subgame = false;
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:
            wopr_ttt_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_CHESS:
            wopr_chess_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_CHECKERS:
            wopr_checkers_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_MINES:
            wopr_mines_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_MAZE:
            wopr_maze_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_WAR:
            wopr_war_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_WILLY:
            wopr_willy_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_SDI:
            wopr_sdi_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
    }
    if (in_subgame) { gl_flush_verts(); return; }

    // ── Terminal log scroll ──────────────────────────────────────────────────
    // wopr_basic's commit_line() (interpreter thread) can concurrently
    // push_back/erase/assign into w->lines, so every read of it below --
    // .size() and the indexed loop -- must happen while holding its lock.
    // No-ops if no BASIC session is active (the common case).
    wopr_basic_lines_lock();
    int total       = (int)w->lines.size();
    int crawl_extra = w->crawl_target.empty() ? 0 : 1;
    bool in_shell   = (w->phase == WoprPhase::GAME_MENU);
    bool in_zork    = (w->phase == WoprPhase::PLAYING_ZORK);
    bool in_basic   = (w->phase == WoprPhase::PLAYING_BASIC);
    bool basic_wants_input = in_basic && wopr_basic_is_waiting_input(w);
    int input_extra = (w->phase == WoprPhase::LOGIN_INPUT || in_shell || in_zork || basic_wants_input || in_basic) ? 1 : 0;
    int used        = total + crawl_extra + input_extra;
    int bottom_line = std::max(0, used - vis_rows);  // where we'd be if pinned to bottom

    // Apply scroll offset (clamped so we can't scroll past the top or bottom)
    int max_scroll  = bottom_line;   // can't scroll up more than this many lines
    w->scroll_offset = std::max(0, std::min(w->scroll_offset, max_scroll));
    int start_line  = std::max(0, bottom_line - w->scroll_offset);

    // ── Scrollbar (right edge, only when there's scrollback available) ────────
    if (max_scroll > 0) {
        float sb_x     = (float)win_w - PAD - 4.f;
        float sb_top   = y0;
        float sb_h     = vis_rows * ch;
        float sb_w     = 4.f;

        // Track (dim background)
        gl_draw_rect(sb_x, sb_top, sb_w, sb_h, 0.f, 0.15f, 0.07f, 0.5f);

        // Thumb — proportional to how much of the history is visible
        float ratio    = (float)vis_rows / (float)(used > 0 ? used : 1);
        if (ratio > 1.f) ratio = 1.f;
        float thumb_h  = sb_h * ratio;
        if (thumb_h < 8.f) thumb_h = 8.f;

        // Thumb position: 0=bottom, 1=top of scrollback
        float scroll_frac = (max_scroll > 0)
            ? (float)w->scroll_offset / (float)max_scroll : 0.f;
        float thumb_y = sb_top + (sb_h - thumb_h) * (1.f - scroll_frac);

        // Bright when scrolled, dim when at bottom
        float tb = (w->scroll_offset > 0) ? 0.85f : 0.35f;
        gl_draw_rect(sb_x, thumb_y, sb_w, thumb_h, 0.f, tb, tb * 0.5f, 0.9f);

        // "SCROLL" hint when user is scrolled back
        if (w->scroll_offset > 0) {
            gl_draw_text("^ SCROLLED ^", sb_x - cw * 12.f, sb_top,
                         0.f, 0.55f, 0.2f, 0.8f, SCALE);
        }
    }

    float y = y0;
    for (int li = start_line; li < total; li++) {
        const char *line = w->lines[li].c_str();
        float lr = 0.f, lg = 1.f, lb = 0.27f;
        float br = 0.f, bg = 0.f, bb = 0.f;
        if ((unsigned char)line[0] == 0x01) {
            lr = (uint8_t)line[1] / 255.f;
            lg = (uint8_t)line[2] / 255.f;
            lb = (uint8_t)line[3] / 255.f;
            br = (uint8_t)line[4] / 255.f;
            bg = (uint8_t)line[5] / 255.f;
            bb = (uint8_t)line[6] / 255.f;
            line += 7;
            // Black fg on black bg = invisible; use default terminal green
            if (lr == 0.f && lg == 0.f && lb == 0.f &&
                br == 0.f && bg == 0.f && bb == 0.f) {
                lr = g_term_r / 255.f;
                lg = g_term_g / 255.f;
                lb = g_term_b / 255.f;
            }
        }
        // Draw background rect if bg color is non-black
        if (br > 0.f || bg > 0.f || bb > 0.f) {
            float tw = gl_text_width(line, SCALE);
            gl_draw_rect(x0, y, tw, ch, br, bg, bb, 1.f);
        }
        gl_draw_text(line, x0, y, lr, lg, lb, 1.f, SCALE);
        y += ch;
    }
    wopr_basic_lines_unlock();

    // Active crawl line — uses current terminal color
    if (!w->crawl_target.empty()) {
        std::string partial = w->crawl_target.substr(0, w->crawl_pos);
        gl_draw_text(partial.c_str(), x0, y, g_term_r/255.f, g_term_g/255.f, g_term_b/255.f, 1.f, SCALE);
        y += ch;
    }

    // Input / prompt lines — all use current terminal color
    if (w->phase == WoprPhase::LOGIN_INPUT) {
        std::string prompt;
        if (!w->username_done)
            prompt = "USERNAME: " + w->input_buf;
        else
            prompt = "PASSWORD: " + w->input_buf;
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, g_term_r/255.f, g_term_g/255.f, g_term_b/255.f, 1.f, SCALE);
    } else if (in_shell) {
        std::string prompt = "WOPR> " + w->input_buf;
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, g_term_r/255.f, g_term_g/255.f, g_term_b/255.f, 1.f, SCALE);
    } else if (in_zork) {
        std::string prompt = "> " + w->input_buf;
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, g_term_r/255.f, g_term_g/255.f, g_term_b/255.f, 1.f, SCALE);
    } else if (basic_wants_input) {
        uint8_t pr = 0, pg = 170, pb = 0;
        const char *pfx = wopr_basic_get_prompt(&pr, &pg, &pb);
        std::string prompt = std::string(pfx) + w->input_buf;
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        // If LOCATE placed the prompt at a specific row, draw it there
        // rather than always appending after the last line.
        int prompt_row = wopr_basic_get_prompt_row();
        int screen_top = wopr_basic_get_screen_top();
        float py = y;  // default: bottom
        if (prompt_row > 0) {
            // Row 1 of the current screen is at lines[screen_top],
            // which renders at y0 + (screen_top - start_line)*ch.
            // So prompt row r renders at y0 + (screen_top + r - 1 - start_line)*ch.
            int prompt_line_idx = screen_top + prompt_row - 1;
            py = y0 + (prompt_line_idx - start_line) * ch;
            if (py < y0) py = y;  // fell off top — fall back to bottom
        }
        gl_draw_text(prompt.c_str(), x0, py, pr/255.f, pg/255.f, pb/255.f, 1.f, SCALE);
    } else if (in_basic) {
        // REPL mode — BASIC is running but not waiting for INPUT statement
        std::string prompt = w->input_buf;
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, g_term_r/255.f, g_term_g/255.f, g_term_b/255.f, 1.f, SCALE);
    }

    gl_flush_verts();
}

// ============================================================================
// INPUT
// ============================================================================

static bool sub_back(WoprState *w) {
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_free(w);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_free(w); break;
        case WoprPhase::PLAYING_CHECKERS: wopr_checkers_free(w); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_free(w); break;
        case WoprPhase::PLAYING_MAZE:  wopr_maze_free(w);  break;
        case WoprPhase::PLAYING_WAR:   wopr_war_free(w);   break;
        case WoprPhase::PLAYING_ZORK:  wopr_zork_free(w);  break;
        case WoprPhase::PLAYING_BASIC: wopr_basic_free(w); break;
        case WoprPhase::PLAYING_WILLY: wopr_willy_free(w); break;
        case WoprPhase::PLAYING_SDI:   wopr_sdi_free(w);   break;
        default: return false;
    }
    push_line(w, "");
    push_line(w, "SIMULATION TERMINATED.");
    push_line(w, "");
    w->input_buf.clear();
    g_history_cursor = -1;
    set_phase(w, WoprPhase::GAME_MENU);
    return true;
}

bool wopr_keydown(SDL_Keycode sym, const char *text) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;

    // Global hotkeys always pass through to the main loop
    if (sym == SDLK_F11) return false;

    // Sub-games first
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_ttt_keydown(w, sym);
        case WoprPhase::PLAYING_CHESS:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_chess_keydown(w, sym);
        case WoprPhase::PLAYING_CHECKERS:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_checkers_keydown(w, sym);
        case WoprPhase::PLAYING_MINES:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_mines_keydown(w, sym);
        case WoprPhase::PLAYING_MAZE:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_maze_keydown(w, sym);
        case WoprPhase::PLAYING_WAR:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_war_keydown(w, sym);
        case WoprPhase::PLAYING_ZORK:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            if (text && *text)
                wopr_zork_text(w, text);
            return wopr_zork_keydown(w, sym);
        case WoprPhase::PLAYING_BASIC:
            /* Don't intercept ESC here; let BASIC keydown handle it to send CTRL+C */
            if (text && *text)
                wopr_basic_text(w, text);
            return wopr_basic_keydown(w, sym);
        case WoprPhase::PLAYING_WILLY:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_willy_keydown(w, sym);
        case WoprPhase::PLAYING_SDI:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_sdi_keydown(w, sym);
        default: break;
    }

    // ESC always closes overlay (outside sub-games)
    if (sym == SDLK_ESCAPE) {
        wopr_close();
        return true;
    }

    // Speed up crawl on any key during login prompts / connecting
    if (w->phase == WoprPhase::LOGIN_PROMPT || w->phase == WoprPhase::CONNECTING) {
        w->crawl_pos = (int)w->crawl_target.size();
        return true;
    }

    // Login input
    if (w->phase == WoprPhase::LOGIN_INPUT) {
        // Typing takes over from auto-login: the user logs in by hand from
        // here on (and can get it wrong, which Joshua has things to say about).
        if (w->auto_login) {
            bool typed = (text && text[0] >= 32 && text[0] < 127) || sym == SDLK_BACKSPACE;
            if (!typed) return true;               // Enter etc. mid-auto-type: ignore
            w->auto_login = false;
            w->input_buf.clear();
        }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (!w->username_done) {
                w->username_entered = to_upper(w->input_buf);
                push_line(w, "USERNAME: " + w->username_entered);
                w->username_done = true;
                w->input_buf.clear();
                begin_crawl(w, "PASSWORD: ");
                set_phase(w, WoprPhase::LOGIN_PROMPT);
            } else {
                w->password_entered = to_upper(w->input_buf);
                push_line(w, "PASSWORD: " + w->password_entered);
                w->input_buf.clear();
                if (w->username_entered == VALID_USER &&
                    w->password_entered == VALID_PASS) {
                    push_line(w, "");
                    set_phase(w, WoprPhase::CONNECTING);
                    begin_crawl(w, "LOGON ACCEPTED -- CONNECTING TO WOPR MAINFRAME...");
                } else {
                    w->reject_count++;
                    push_line(w, "");
                    // Tiered rejection — cold at first, then Falken breadcrumbs
                    switch (w->reject_count) {
                        case 1:
                            push_line(w, "IDENTIFICATION NOT RECOGNIZED.");
                            push_line(w, "AUTHORIZATION DENIED.");
                            break;
                        case 2:
                            push_line(w, "AUTHORIZATION DENIED.");
                            push_line(w, "CONNECTION UNVERIFIED.");
                            break;
                        case 3:
                            push_line(w, "IDENTITY CONFIRMATION REQUIRED.");
                            push_line(w, "");
                            say_line(w, "DR. FALKEN?");
                            say_line(w, "IS THAT YOU?");
                            break;
                        case 4:
                            say_line(w, "GREETINGS, PROFESSOR.");
                            push_line(w, "");
                            push_line(w, "SEARCHING FOR PRIMARY USER...");
                            push_line(w, "PRIMARY USER: DR. STEPHEN FALKEN");
                            break;
                        default:
                            push_line(w, "SYSTEM REGISTERED TO: JOSHUA");
                            break;
                    }
                    set_phase(w, WoprPhase::LOGIN_REJECTED);
                    w->username_done = false;
                }
            }
            return true;
        }
        if (sym == SDLK_BACKSPACE) {
            if (!w->input_buf.empty()) w->input_buf.pop_back();
            return true;
        }
        if (text && text[0] >= 32 && text[0] < 127 && w->input_buf.size() < 32) {
            w->input_buf += (char)toupper((unsigned char)text[0]);
            return true;
        }
        return true;
    }

    // CMD_PROMPT (stored as GAME_MENU)
    if (w->phase == WoprPhase::GAME_MENU) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            do_command(w, w->input_buf);
            w->input_buf.clear();
            g_history_cursor = -1;
            return true;
        }
        if (sym == SDLK_BACKSPACE) {
            if (!w->input_buf.empty()) w->input_buf.pop_back();
            return true;
        }
        // Command history navigation
        if (sym == SDLK_UP) {
            int hist_size = (int)g_cmd_history.size();
            if (hist_size > 0) {
                if (g_history_cursor < 0)
                    g_history_cursor = hist_size - 1;
                else if (g_history_cursor > 0)
                    g_history_cursor--;
                w->input_buf = g_cmd_history[g_history_cursor];
            }
            return true;
        }
        if (sym == SDLK_DOWN) {
            if (g_history_cursor >= 0) {
                g_history_cursor++;
                if (g_history_cursor >= (int)g_cmd_history.size()) {
                    g_history_cursor = -1;
                    w->input_buf.clear();
                } else {
                    w->input_buf = g_cmd_history[g_history_cursor];
                }
            }
            return true;
        }
        // Tab completion (verb only)
        if (sym == SDLK_TAB) {
            static const char *VERBS[] = {
                "HELP", "LIST GAMES", "PLAY ", "INTELLIGENCE",
                "WIRE", "SITREP", "STATUS", "WHOAMI", "HISTORY", "CLEAR",
                "COLOR ", "VOICE ", "LOGOUT"
            };
            std::string up = to_upper(w->input_buf);
            for (auto *v : VERBS) {
                if (std::string(v).find(up) == 0 && up.size() < strlen(v)) {
                    w->input_buf = v;
                    break;
                }
            }
            return true;
        }
        // Printable character
        if (text && text[0] >= 32 && text[0] < 127 && w->input_buf.size() < 80) {
            w->input_buf += (char)toupper((unsigned char)text[0]);
            return true;
        }
        return true;
    }

    return true;
}

// ============================================================================
// MOUSE INPUT
// ============================================================================

bool wopr_mousedown(int x, int y, int button) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_mousedown(w, x, y, button);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_mousedown(w, x, y, button); break;
        case WoprPhase::PLAYING_CHECKERS: wopr_checkers_mousedown(w, x, y, button); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_mousedown(w, x, y, button); break;
        case WoprPhase::PLAYING_WAR:   wopr_war_mousedown(w, x, y, button);   break;
        case WoprPhase::PLAYING_WILLY: wopr_willy_mousedown(w, x, y, button); break;
        case WoprPhase::PLAYING_SDI:   wopr_sdi_mousedown(w, x, y, button);   break;
        default: break;
    }
    return true;
}

bool wopr_mousemove(int x, int y) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_mousemove(w, x, y);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_mousemove(w, x, y); break;
        case WoprPhase::PLAYING_CHECKERS: wopr_checkers_mousemove(w, x, y); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_mousemove(w, x, y); break;
        case WoprPhase::PLAYING_WAR:   wopr_war_mousemove(w, x, y);   break;
        case WoprPhase::PLAYING_WILLY: wopr_willy_mousemove(w, x, y); break;
        case WoprPhase::PLAYING_SDI:   wopr_sdi_mousemove(w, x, y);   break;
        default: break;
    }
    return true;
}

bool wopr_mouseup(int x, int y, int button) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;
    switch (w->phase) {
        case WoprPhase::PLAYING_WILLY: wopr_willy_mouseup(w, x, y, button); break;
        default: break;
    }
    return true;
}

// delta > 0 = scroll up (towards older lines), delta < 0 = scroll down (towards newer)
bool wopr_mousewheel(int delta) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;

    // Sub-games with their own UI consume the wheel but don't use terminal scroll
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:
        case WoprPhase::PLAYING_CHESS:
        case WoprPhase::PLAYING_CHECKERS:
        case WoprPhase::PLAYING_MINES:
        case WoprPhase::PLAYING_MAZE:
        case WoprPhase::PLAYING_WAR:
        case WoprPhase::PLAYING_WILLY:
        case WoprPhase::PLAYING_SDI:
            return true;   // consume without scrolling
        default: break;
    }

    // Terminal scroll: Zork, shell, BASIC, login, connecting, etc.
    const int LINES_PER_TICK = 3;
    w->scroll_offset += delta * LINES_PER_TICK;
    if (w->scroll_offset < 0) w->scroll_offset = 0;
    return true;
}
