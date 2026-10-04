/*
 * sound_sdl.c — SDL2 audio backend for the BASIC interpreter.
 *
 * Synthesis model: square wave, 44100 Hz mono Sint16.
 * A simple queue of (frequency, duration_ms, gap_ms) events is fed to
 * the SDL audio callback.  PLAY MF (foreground) blocks until the queue
 * drains; PLAY MB (background, default) returns immediately.
 *
 * Build flag: -DHAVE_SDL  (set by Makefile when SDL2 is present)
 */
#ifdef HAVE_SDL

#include "sound.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

BASIC_NS_BEGIN

/* ================================================================
 * Audio parameters
 * ================================================================ */
#define SAMPLE_RATE   44100
#define CHANNELS      1
#define BUFFER_FRAMES 512        /* SDL callback buffer size in frames */

/* ================================================================
 * Note event queue
 * ================================================================ */
#define MAX_QUEUE 1024

typedef struct {
    double freq;        /* Hz; 0 = rest */
    int    tone_ms;     /* duration of tone (or silence for rest) */
    int    gap_ms;      /* silence after tone (staccato gap / articulation) */
} NoteEvent;

static NoteEvent  g_queue[MAX_QUEUE];
static int        g_q_head = 0;   /* next to consume */
static int        g_q_tail = 0;   /* next to write   */
static SDL_mutex *g_mutex  = NULL;
static SDL_cond  *g_cond   = NULL;

static int q_count(void) {
    return (g_q_tail - g_q_head + MAX_QUEUE) % MAX_QUEUE;
}
static int q_full(void) { return q_count() >= MAX_QUEUE - 1; }

/* Enqueue one note (called from main thread, under mutex). */
static void q_push(double freq, int tone_ms, int gap_ms) {
    SDL_LockMutex(g_mutex);
    while (q_full()) SDL_CondWait(g_cond, g_mutex); /* back-pressure */
    NoteEvent ev = { freq, tone_ms, gap_ms };
    g_queue[g_q_tail] = ev;
    g_q_tail = (g_q_tail + 1) % MAX_QUEUE;
    SDL_UnlockMutex(g_mutex);
}

/* ================================================================
 * SDL audio callback — runs on audio thread
 * ================================================================ */
typedef struct {
    double   phase;          /* current phase [0, 1) */
    double   freq;           /* current frequency    */
    int      samples_left;   /* samples remaining in current segment */
    int      in_gap;         /* 1 if we're in the post-note gap      */
    int      gap_left;       /* samples remaining in gap             */
    NoteEvent cur;           /* note currently playing               */
    int       have_note;     /* 1 if cur is valid                    */
} SynthState;

static SynthState g_synth = {};

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    Sint16 *out   = (Sint16 *)stream;
    int     nfr   = len / sizeof(Sint16);
    SynthState *s = &g_synth;

    SDL_LockMutex(g_mutex);

    for (int i = 0; i < nfr; ) {
        /* Refill current note if needed */
        if (!s->have_note || (s->samples_left <= 0 && s->gap_left <= 0)) {
            if (q_count() > 0) {
                s->cur       = g_queue[g_q_head];
                g_q_head     = (g_q_head + 1) % MAX_QUEUE;
                s->samples_left = (int)((double)s->cur.tone_ms * SAMPLE_RATE / 1000.0);
                s->gap_left     = (int)((double)s->cur.gap_ms  * SAMPLE_RATE / 1000.0);
                s->in_gap    = 0;
                s->have_note = 1;
                s->freq      = s->cur.freq;
                SDL_CondSignal(g_cond); /* wake producer if it was waiting */
            } else {
                /* queue empty — output silence */
                out[i++] = 0;
                continue;
            }
        }

        /* In post-note gap: output silence */
        if (s->in_gap) {
            int chunk = s->gap_left < (nfr - i) ? s->gap_left : (nfr - i);
            memset(out + i, 0, chunk * sizeof(Sint16));
            i             += chunk;
            s->gap_left   -= chunk;
            if (s->gap_left <= 0) s->have_note = 0;
            continue;
        }

        /* Generate square-wave samples */
        int chunk = s->samples_left < (nfr - i) ? s->samples_left : (nfr - i);
        if (s->freq < 1.0) {
            /* rest — silence */
            memset(out + i, 0, chunk * sizeof(Sint16));
        } else {
            double phase_inc = s->freq / SAMPLE_RATE;
            for (int j = 0; j < chunk; j++) {
                out[i + j] = (s->phase < 0.5) ? 20000 : -20000;
                s->phase   += phase_inc;
                if (s->phase >= 1.0) s->phase -= 1.0;
            }
        }
        i               += chunk;
        s->samples_left -= chunk;

        if (s->samples_left <= 0) {
            if (s->gap_left > 0) s->in_gap = 1;
            else                 s->have_note = 0;
        }
    }

    SDL_UnlockMutex(g_mutex);
}

/* ================================================================
 * Init / shutdown
 * ================================================================ */
static SDL_AudioDeviceID g_dev = 0;

void sound_init(void) {
    /* If already initialised (e.g. re-entering BASIC without a clean shutdown),
     * tear down first so we don't open a second device on top of the first. */
    if (g_dev) {
        sound_shutdown();
    }

    /* Reset queue and synth state from any previous session.
     * In a standalone build these are always zero-initialised, but in WOPR
     * the statics survive across enter/free cycles.  A stale g_synth.have_note
     * or mismatched queue indices will cause the callback to skip new notes. */
    g_q_head = 0;
    g_q_tail = 0;
    memset(&g_synth, 0, sizeof(g_synth));

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        basic_stderr("sound: SDL_InitSubSystem: %s\n", SDL_GetError());
        return;
    }
    g_mutex = SDL_CreateMutex();
    g_cond  = SDL_CreateCond();

    SDL_AudioSpec want = {}, have;
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples  = BUFFER_FRAMES;
    want.callback = audio_callback;

    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        basic_stderr("sound: SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_dev, 0); /* start playing */
}

void sound_drain(void) {
    if (!g_dev || !g_mutex) return;
    while (1) {
        SDL_LockMutex(g_mutex);
        int empty = (q_count() == 0 && !g_synth.have_note);
        SDL_UnlockMutex(g_mutex);
        if (empty) break;
        SDL_Delay(5);
    }
}

void sound_shutdown(void) {
    sound_drain();  /* wait for queued audio to finish before tearing down */
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
    if (g_cond)  { SDL_DestroyCond(g_cond);   g_cond  = NULL; }
    if (g_mutex) { SDL_DestroyMutex(g_mutex); g_mutex = NULL; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/* ================================================================
 * BEEP
 * ================================================================ */
void sound_beep(void) {
    sound_tone(800.0, 4.55);   /* 4.55 ticks ≈ 0.25 s at 18.2 ticks/s */
    sound_drain();
}

/* ================================================================
 * SOUND freq, duration_ticks
 * ================================================================ */
void sound_tone(double freq, double duration_ticks) {
    if (!g_dev) return;
    int ms = (int)(duration_ticks / 18.2 * 1000.0);
    if (ms < 1) ms = 1;
    q_push(freq, ms, 0);
}

/* ================================================================
 * PLAY — Music Macro Language parser
 * ================================================================ */

/* Semitone offsets from C for each note letter (A=0 index) */
static const int note_semitone[7] = {
    9,  /* A */
    11, /* B */
    0,  /* C */
    2,  /* D */
    4,  /* E */
    5,  /* F */
    7,  /* G */
};

/* Frequency for MIDI note number n (A4=440, MIDI 69) */
static double midi_to_freq(int midi) {
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

/* Note letter index: A=0 B=1 C=2 D=3 E=4 F=5 G=6, or -1 */
static int note_index(char c) {
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'G') return c - 'A';
    return -1;
}

void sound_play(char *mml) {
    if (!g_dev || !mml) return;

    /* Playback state */
    int    octave     = 4;      /* default octave (O4) */
    int    length     = 4;      /* default note length (quarter) */
    int    tempo      = 120;    /* BPM */
    int    foreground = 0;      /* 0=background, 1=foreground */
    /* Articulation: fraction of note duration that is tone vs gap */
    double tone_frac  = 7.0/8.0;  /* MN normal */

    char *p = mml;

    while (*p) {
        /* Skip whitespace and semicolons */
        while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (!*p) break;

        char cmd = (char)toupper((unsigned char)*p++);

        if (cmd == 'O') {
            /* Octave: O0-O6 */
            if (isdigit((unsigned char)*p)) {
                octave = *p++ - '0';
                if (octave < 0) octave = 0;
                if (octave > 6) octave = 6;
            }

        } else if (cmd == '>') {
            if (octave < 6) octave++;

        } else if (cmd == '<') {
            if (octave > 0) octave--;

        } else if (cmd == 'L') {
            /* Note length */
            int n = 0;
            while (isdigit((unsigned char)*p)) n = n * 10 + (*p++ - '0');
            if (n >= 1 && n <= 64) length = n;

        } else if (cmd == 'T') {
            /* Tempo in BPM */
            int n = 0;
            while (isdigit((unsigned char)*p)) n = n * 10 + (*p++ - '0');
            if (n >= 32 && n <= 255) tempo = n;

        } else if (cmd == 'M') {
            /* Mode: MN ML MS MF MB */
            char sub = (char)toupper((unsigned char)*p);
            if (sub == 'N') { tone_frac = 7.0/8.0; p++; }
            else if (sub == 'L') { tone_frac = 1.0;     p++; }
            else if (sub == 'S') { tone_frac = 3.0/4.0; p++; }
            else if (sub == 'F') { foreground = 1;       p++; }
            else if (sub == 'B') { foreground = 0;       p++; }

        } else if (cmd == 'P') {
            /* Pause / rest */
            int dur = 0;
            while (isdigit((unsigned char)*p)) dur = dur * 10 + (*p++ - '0');
            if (dur < 1 || dur > 64) dur = length;
            /* dotted? */
            double ms = (60000.0 / tempo) * (4.0 / dur);
            if (*p == '.') { ms *= 1.5; p++; }
            q_push(0.0, (int)ms, 0);

        } else if (cmd == 'N') {
            /* Absolute note number 0-84 (0=rest) */
            int n = 0;
            while (isdigit((unsigned char)*p)) n = n * 10 + (*p++ - '0');
            double ms = (60000.0 / tempo) * (4.0 / length);
            if (*p == '.') { ms *= 1.5; p++; }
            int tone_ms = (int)(ms * tone_frac);
            int gap_ms  = (int)(ms * (1.0 - tone_frac));
            /* N1 = O0 C, N84 = O6 B: same scale as the letter notes */
            if (n == 0 || n > 84) q_push(0.0, (int)ms, 0);
            else                  q_push(midi_to_freq(n + 35), tone_ms, gap_ms);

        } else {
            /* Note: A-G with optional sharp/flat and optional length */
            int ni = note_index(cmd);
            if (ni < 0) continue; /* unknown command — skip */

            /* Sharp / flat */
            int semitone = note_semitone[ni];
            if (*p == '#' || *p == '+') { semitone++; p++; }
            else if (*p == '-')         { semitone--; p++; }

            /* Optional explicit length */
            int dur = 0;
            while (isdigit((unsigned char)*p)) dur = dur * 10 + (*p++ - '0');
            if (dur < 1 || dur > 64) dur = length;

            /* Dotted note? */
            double ms = (60000.0 / tempo) * (4.0 / dur);
            if (*p == '.') { ms *= 1.5; p++; }

            /* QBasic's pitch: O2 C is middle C (MIDI 60) and O2 A is
             * 440 Hz, the same scale QB64 uses -- so the default O4 is two
             * octaves above middle C, not at it. */
            int midi = (octave + 3) * 12 + semitone;
            if (midi < 0)  midi = 0;
            if (midi > 127) midi = 127;

            int tone_ms = (int)(ms * tone_frac);
            int gap_ms  = (int)(ms * (1.0 - tone_frac));
            if (tone_ms < 1) tone_ms = 1;

            q_push(midi_to_freq(midi), tone_ms, gap_ms);
        }
    }

    if (foreground) sound_drain();
}

BASIC_NS_END

#endif /* HAVE_SDL */
