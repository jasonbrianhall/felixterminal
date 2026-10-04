// mml_player.cpp — QB/GW-BASIC MML player for felix terminal
//
// Fits the existing crt_audio architecture: F32 mono @ 44100 Hz,
// SDL_LockAudioDevice / SDL_UnlockAudioDevice for thread safety.
// Mixes into the shared audio device via term_audio_get_device().

#include "mml_player.h"
#include "crt_audio.h"

#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <string>
#include <vector>

// ============================================================================
// CONSTANTS
// ============================================================================

#define MML_SAMPLE_RATE  44100
#define MML_MAX_NOTES    1024
#define MML_VOICES       3       // polyphony for chords (MML uses ; to separate voices)

static const float TWOPI = 6.28318530f;

// ============================================================================
// NOTE EVENT
// ============================================================================

struct MmlNote {
    float freq;          // Hz, 0 = rest
    int   samples_total; // duration in samples
    float volume;        // 0..1
    float articulation;  // 1.0=legato, 0.875=normal, 0.75=staccato (gate ratio)
};

// ============================================================================
// VOICE STATE (rendered in audio callback)
// ============================================================================

struct MmlVoice {
    // Playback
    std::vector<MmlNote> notes;
    int   note_idx       = 0;
    int   samples_left   = 0;   // samples left in current note's gate-on phase
    int   rest_left      = 0;   // samples left in current note's gate-off (release) phase
    float phase          = 0.f;
    float freq           = 0.f;
    float volume         = 0.f;
    float articulation   = 0.875f;
    bool  playing        = false;

    void reset() {
        notes.clear();
        note_idx = 0; samples_left = 0; rest_left = 0;
        phase = 0.f; freq = 0.f; volume = 0.f; playing = false;
    }
};

static MmlVoice s_voices[MML_VOICES];
static SDL_AudioDeviceID s_dev = 0;

// ============================================================================
// WAVEFORM — square wave with slight pulse-width variation for warmth
// ============================================================================

static inline float mml_wave(float phase, float /*freq*/) {
    // Square wave (classic BASIC sound)
    return (phase < TWOPI * 0.5f) ? 0.4f : -0.4f;
}

// ============================================================================
// AUDIO CALLBACK MIXING — called by the SDL audio thread
// ============================================================================

// This is called from inside the existing audio callback via mml_mix().
// We expose a single function that crt_audio.cpp can call at the end of
// its callback, OR we hook in via the wopr_play mechanism (pre-mixed F32 buf).
// Since we can't modify crt_audio.cpp's callback directly, we use the
// same pre-rendered buffer approach as WOPR: render ahead into a ring buffer.

#define MML_RING_SAMPLES  (MML_SAMPLE_RATE * 4)   // 4 second ring buffer

static float    s_ring[MML_RING_SAMPLES] = {};
static volatile int s_ring_write = 0;   // written by render thread
static volatile int s_ring_read  = 0;   // read by audio callback
static volatile int s_ring_fill  = 0;   // samples available

// Render thread fills the ring; audio callback drains it.
// We render on demand from mml_play() and from a background fill call.

static void ring_write_samples(const float *buf, int n) {
    for (int i = 0; i < n; i++) {
        if (s_ring_fill >= MML_RING_SAMPLES) break;
        s_ring[s_ring_write % MML_RING_SAMPLES] = buf[i];
        s_ring_write = s_ring_write + 1;
        s_ring_fill  = s_ring_fill + 1;
    }
}

// Render up to `n` samples from voice state into dst. Returns samples rendered.
static int render_voice(MmlVoice &v, float *dst, int n) {
    int rendered = 0;
    while (rendered < n && v.playing) {
        if (v.note_idx >= (int)v.notes.size()) {
            v.playing = false;
            break;
        }

        const MmlNote &note = v.notes[v.note_idx];

        // Start of a new note
        if (v.samples_left == 0 && v.rest_left == 0) {
            int gate_on  = (int)(note.samples_total * note.articulation);
            int gate_off = note.samples_total - gate_on;
            v.freq        = note.freq;
            v.volume      = note.volume;
            v.samples_left = gate_on  > 0 ? gate_on  : 1;
            v.rest_left    = gate_off > 0 ? gate_off : 0;
            if (note.freq <= 0.f) { v.samples_left = note.samples_total; v.rest_left = 0; }
        }

        if (v.samples_left > 0) {
            // Gate-on: produce tone
            int chunk = SDL_min(v.samples_left, n - rendered);
            for (int i = 0; i < chunk; i++) {
                float s = 0.f;
                if (note.freq > 0.f) {
                    s = mml_wave(v.phase, v.freq) * v.volume;
                    // Simple exponential decay within note for pluck feel
                    float t = 1.f - (float)v.samples_left / (float)note.samples_total;
                    s *= expf(-t * 1.5f);
                    v.phase = fmodf(v.phase + TWOPI * v.freq / MML_SAMPLE_RATE, TWOPI);
                }
                dst[rendered++] += s;
                v.samples_left--;
            }
        } else if (v.rest_left > 0) {
            // Gate-off: silence (articulation gap)
            int chunk = SDL_min(v.rest_left, n - rendered);
            rendered     += chunk;
            v.rest_left  -= chunk;
            if (v.rest_left == 0) {
                v.note_idx++;
                // Reset for next note
                v.samples_left = 0;
            }
        }

        if (v.samples_left == 0 && v.rest_left == 0) {
            v.note_idx++;
        }
    }
    return rendered;
}

// Fill `n` samples into dst (additive mix of all voices)
static void mml_render_block(float *dst, int n) {
    memset(dst, 0, n * sizeof(float));
    bool any = false;
    for (int vi = 0; vi < MML_VOICES; vi++) {
        if (s_voices[vi].playing) { render_voice(s_voices[vi], dst, n); any = true; }
    }
    (void)any;
    // Soft clip
    for (int i = 0; i < n; i++) {
        float s = dst[i];
        if (s >  1.f) s =  1.f;
        if (s < -1.f) s = -1.f;
        dst[i] = s;
    }
}

// ============================================================================
// MML PARSER
// ============================================================================

// MIDI note number to frequency
static float midi_to_freq(int midi) {
    return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

// Note letter to semitone offset (C=0)
static int note_letter_semi(char c) {
    switch (toupper(c)) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default:  return 0;
    }
}

struct MmlState {
    int   octave      = 4;
    int   tempo       = 120;   // BPM
    int   length      = 4;    // default note length
    int   volume      = 8;    // 0-15
    float articulation = 0.875f; // MN
};

// Samples per beat at given tempo
static int samples_per_beat(int tempo) {
    return (int)((float)MML_SAMPLE_RATE * 60.f / (float)tempo);
}

// Duration in samples for a note of given length (1=whole, 2=half, 4=quarter...)
static int note_duration(int length, bool dotted, int tempo) {
    int spb = samples_per_beat(tempo);
    // One beat = quarter note (length=4)
    int samples = spb * 4 / length;
    if (dotted) samples = samples * 3 / 2;
    return samples;
}

static int parse_int(const char *p, int *out, int def) {
    int val = 0; int n = 0;
    while (isdigit((unsigned char)p[n])) { val = val * 10 + (p[n] - '0'); n++; }
    *out = n > 0 ? val : def;
    return n;
}

static std::vector<MmlNote> parse_mml(const char *mml, MmlState &st) {
    std::vector<MmlNote> notes;
    const char *p = mml;

    while (*p) {
        // Skip whitespace
        if (isspace((unsigned char)*p)) { p++; continue; }

        char c = toupper((unsigned char)*p);

        if (c == 'M') {
            p++;
            char m = toupper((unsigned char)*p);
            if      (m == 'N') { st.articulation = 0.875f; p++; }
            else if (m == 'L') { st.articulation = 1.0f;   p++; }
            else if (m == 'S') { st.articulation = 0.5f;   p++; }
            else if (m == 'B' || m == 'F') { p++; } // background/foreground: ignore
            continue;
        }

        if (c == 'T') {
            p++;
            int val; p += parse_int(p, &val, 120);
            if (val < 32) val = 32; if (val > 255) val = 255;
            st.tempo = val;
            continue;
        }

        if (c == 'O') {
            p++;
            int val; p += parse_int(p, &val, 4);
            if (val < 0) val = 0; if (val > 7) val = 7;
            st.octave = val;
            continue;
        }

        if (c == '<') { if (st.octave > 0) st.octave--; p++; continue; }
        if (c == '>') { if (st.octave < 7) st.octave++; p++; continue; }

        if (c == 'L') {
            p++;
            int val; p += parse_int(p, &val, 4);
            if (val >= 1 && val <= 64) st.length = val;
            continue;
        }

        if (c == 'V') {
            p++;
            int val; p += parse_int(p, &val, 8);
            if (val < 0) val = 0; if (val > 15) val = 15;
            st.volume = val;
            continue;
        }

        if (c == 'N') {
            p++;
            int midi; p += parse_int(p, &midi, 0);
            // Optional length
            int len = st.length; bool dot = false;
            if (*p == ',') { p++; p += parse_int(p, &len, st.length); }
            if (*p == '.') { dot = true; p++; }
            MmlNote n;
            // N1 = O0 C ... N84 = O6 B, on the same scale as the letters
            n.freq          = (midi > 0 && midi <= 84) ? midi_to_freq(midi + 35) : 0.f;
            n.samples_total = note_duration(len, dot, st.tempo);
            n.volume        = st.volume / 15.f * 0.8f;
            n.articulation  = st.articulation;
            notes.push_back(n);
            continue;
        }

        if (c == 'R' || c == 'P') {
            p++;
            int len; bool dot = false;
            p += parse_int(p, &len, st.length);
            if (*p == '.') { dot = true; p++; }
            MmlNote n;
            n.freq = 0.f;
            n.samples_total = note_duration(len, dot, st.tempo);
            n.volume = 0.f;
            n.articulation = 1.f;
            notes.push_back(n);
            continue;
        }

        // Note: A-G
        if (c >= 'A' && c <= 'G') {
            p++;
            int semi = note_letter_semi(c);

            // Sharp/flat
            if (*p == '#' || *p == '+') { semi++; p++; }
            else if (*p == '-')         { semi--; p++; }

            // Length
            int len = st.length; bool dot = false;
            int tmp; int n_digits = parse_int(p, &tmp, -1);
            if (n_digits > 0 && tmp >= 1 && tmp <= 64) { len = tmp; p += n_digits; }
            if (*p == '.') { dot = true; p++; }

            // QBasic pitch: O2 C = middle C (MIDI 60), O2 A = 440 Hz
            int midi = (st.octave + 3) * 12 + semi;
            if (midi < 0)  midi = 0;
            if (midi > 127) midi = 127;

            MmlNote note;
            note.freq          = midi_to_freq(midi);
            note.samples_total = note_duration(len, dot, st.tempo);
            note.volume        = st.volume / 15.f * 0.8f;
            note.articulation  = st.articulation;
            notes.push_back(note);
            continue;
        }

        // Unknown token — skip
        p++;
    }

    return notes;
}

// ============================================================================
// PUBLIC API
// ============================================================================

void mml_init(void) {
    s_dev = term_audio_get_device();
    for (int i = 0; i < MML_VOICES; i++) s_voices[i].reset();
    s_ring_write = 0;
    s_ring_read  = 0;
    s_ring_fill  = 0;
    memset(s_ring, 0, sizeof(s_ring));
}

void mml_play(const char *mml) {
    if (!s_dev || !mml || !*mml) return;

    SDL_LockAudioDevice(s_dev);

    // Stop all voices
    for (int i = 0; i < MML_VOICES; i++) s_voices[i].reset();

    // Split on ';' for multi-voice (Gorilla.bas doesn't use this but support it)
    // For now parse as single voice
    MmlState st;
    std::vector<MmlNote> notes = parse_mml(mml, st);

    if (!notes.empty()) {
        s_voices[0].notes   = std::move(notes);
        s_voices[0].note_idx = 0;
        s_voices[0].samples_left = 0;
        s_voices[0].rest_left    = 0;
        s_voices[0].phase        = 0.f;
        s_voices[0].playing      = true;
    }

    // Pre-render into ring buffer so the callback always has data
    s_ring_write = 0;
    s_ring_read  = 0;
    s_ring_fill  = 0;
    const int PREFILL = MML_SAMPLE_RATE / 4;  // 250ms prefill
    float tmp[PREFILL];
    mml_render_block(tmp, PREFILL);
    ring_write_samples(tmp, PREFILL);

    SDL_UnlockAudioDevice(s_dev);

    SDL_Log("[MML] play: %d notes queued\n", (int)s_voices[0].notes.size() + PREFILL/MML_SAMPLE_RATE);
}

void mml_stop(void) {
    if (!s_dev) return;
    SDL_LockAudioDevice(s_dev);
    for (int i = 0; i < MML_VOICES; i++) s_voices[i].reset();
    s_ring_fill = 0;
    SDL_UnlockAudioDevice(s_dev);
}

bool mml_is_playing(void) {
    for (int i = 0; i < MML_VOICES; i++)
        if (s_voices[i].playing) return true;
    return false;
}

void mml_shutdown(void) {
    mml_stop();
    s_dev = 0;
}

// ============================================================================
// MIX FUNCTION — call from crt_audio's audio_callback or via WOPR mechanism
// ============================================================================

// Since we can't modify crt_audio.cpp's callback directly, we deliver audio
// the same way WOPR does: render to a float buffer and hand it to
// term_audio_wopr_play(). This works because wopr_play just mixes into the
// existing stream. The downside is we have to pre-render the whole sequence.
// For BASIC MML strings (a few seconds max) this is fine.

void mml_play_via_wopr(const char *mml) {
    if (!s_dev || !mml || !*mml) return;

    MmlState st;
    std::vector<MmlNote> notes = parse_mml(mml, st);
    if (notes.empty()) return;

    // Calculate total samples needed
    int total = 0;
    for (auto &n : notes) total += n.samples_total;
    if (total <= 0 || total > MML_SAMPLE_RATE * 30) return;  // cap at 30s

    float *buf = (float*)malloc(total * sizeof(float));
    if (!buf) return;

    // Render into temporary voice
    MmlVoice v;
    v.notes   = std::move(notes);
    v.note_idx = 0;
    v.samples_left = 0;
    v.rest_left    = 0;
    v.phase   = 0.f;
    v.playing = true;

    memset(buf, 0, total * sizeof(float));
    render_voice(v, buf, total);

    // Soft clip
    for (int i = 0; i < total; i++) {
        if (buf[i] >  1.f) buf[i] =  1.f;
        if (buf[i] < -1.f) buf[i] = -1.f;
    }

    SDL_Log("[MML] rendering %d samples (%.1fs) via wopr channel\n",
            total, (float)total / MML_SAMPLE_RATE);

    // Free previous buffer if still alive, then hand off the new one.
    // We keep a static pointer so we can free it on the next call or shutdown.
    static float *s_mml_buf = nullptr;
    if (s_mml_buf) {
        // Previous playback must be finished before we overwrite — stop it first.
        term_audio_wopr_stop();
        free(s_mml_buf);
    }
    s_mml_buf = buf;
    term_audio_wopr_play(buf, total);
}
