// wopr_speech.cpp — Joshua's voice: the formant synthesizer in synth/synth.c
// speaking WOPR's lines.
//
// Speech plays on its own small SDL audio device (like the keystroke clicks
// in wopr.cpp), so it works whether or not the terminal's CRT sound effects
// are on and never cuts off the modem screech. Lines are queued with
// SDL_QueueAudio and play one after another. Each line is rendered when it
// is said (a few milliseconds) at the device's own sample rate.
//
// The pronouncing dictionary (CMUdict) is built in, compressed, in
// synth/cmudict_z.h. It is unpacked on a background thread when WOPR opens.

#include "wopr.h"
#include "synth/synth.h"
#include "synth/cmudict_z.h"
#include "../felixchirp/miniz.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static SDL_AudioDeviceID s_dev      = 0;
static int               s_rate     = 44100;
static SDL_Thread       *s_loader   = nullptr;
static SDL_atomic_t      s_ready;            // 1 once the dictionary is in
static WoprVoice         s_mode     = WoprVoice::KEY_LINES;

static const double MAX_BACKLOG_SECONDS = 30.0;   // VOICE ALL: don't fall minutes behind

// ── Dictionary ──────────────────────────────────────────────────────────────

static int b64(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int load_dictionary(void *) {
    unsigned char *z = (unsigned char *)malloc(CMUDICT_Z_SIZE + 4);
    unsigned char *raw = (unsigned char *)malloc(CMUDICT_Z_RAW_SIZE + 1);
    mz_ulong zn = 0, rawn = CMUDICT_Z_RAW_SIZE;
    if (z && raw) {
        unsigned acc = 0;
        int bits = 0;
        for (int l = 0; l < CMUDICT_Z_B64_LINES; l++)
            for (const char *p = CMUDICT_Z_B64[l]; *p; p++) {
                int v = b64((unsigned char)*p);
                if (v < 0) continue;                 // '=' padding
                acc = (acc << 6) | (unsigned)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    if (zn < CMUDICT_Z_SIZE + 4) z[zn++] = (unsigned char)(acc >> bits);
                }
            }
        if (mz_uncompress(raw, &rawn, z, zn) == MZ_OK)
            tts_init_text((const char *)raw, (size_t)rawn);
        else {
            SDL_Log("[WOPR voice] dictionary unpack failed; using letter rules only\n");
            tts_init_text(nullptr, 0);
        }
    } else {
        tts_init_text(nullptr, 0);
    }
    free(z);
    free(raw);
    SDL_AtomicSet(&s_ready, 1);
    return 0;
}

void wopr_speech_preload() {
    if (SDL_AtomicGet(&s_ready) || s_loader) return;
    s_loader = SDL_CreateThread(load_dictionary, "wopr-voice-dict", nullptr);
    if (!s_loader) load_dictionary(nullptr);     // no threads: load now
}

static void wait_for_dictionary() {
    if (s_loader) { SDL_WaitThread(s_loader, nullptr); s_loader = nullptr; }
    if (!SDL_AtomicGet(&s_ready)) load_dictionary(nullptr);
}

// ── Audio device ────────────────────────────────────────────────────────────

static bool open_device() {
    if (s_dev) return true;
    SDL_AudioSpec want{}, got{};
    want.freq     = 44100;
    want.format   = AUDIO_F32SYS;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = nullptr;                     // queued: SDL_QueueAudio
    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!s_dev) {
        SDL_Log("[WOPR voice] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    s_rate = got.freq;                           // speak at the device's own rate
    SDL_PauseAudioDevice(s_dev, 0);
    return true;
}

// ── Speaking ────────────────────────────────────────────────────────────────

static void speak(const char *text) {
    if (s_mode == WoprVoice::OFF || !text) return;
    // Lines from push_line may start with a \x01 R G B R G B colour stamp.
    if ((unsigned char)text[0] == 0x01) {
        if (strlen(text) < 7) return;
        text += 7;
    }
    bool any = false;
    for (const char *p = text; *p; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) { any = true; break; }
    if (!any) return;

    wait_for_dictionary();
    if (!open_device()) return;
    if (SDL_GetQueuedAudioSize(s_dev) > (Uint32)(MAX_BACKLOG_SECONDS * s_rate * sizeof(float)))
        return;

    int n = 0;
    float *pcm = tts_render(text, s_rate, &n);
    if (pcm && n > 0) {
        for (int i = 0; i < n; i++) pcm[i] *= 0.8f;   // a little under full scale
        SDL_QueueAudio(s_dev, pcm, (Uint32)(n * sizeof(float)));
        // a short breath between lines
        static float gap[4800];
        int g = s_rate / 8;
        if (g > 4800) g = 4800;
        SDL_QueueAudio(s_dev, gap, (Uint32)(g * sizeof(float)));
    }
    tts_free(pcm);
}

void wopr_say(const char *text) {
    speak(text);
}

void wopr_say_any(const char *text) {
    if (s_mode == WoprVoice::ALL) speak(text);
}

void wopr_speech_stop() {
    if (s_dev) SDL_ClearQueuedAudio(s_dev);
}

bool wopr_speech_busy() {
    return s_dev && SDL_GetQueuedAudioSize(s_dev) > 0;
}

WoprVoice wopr_voice_mode() { return s_mode; }

void wopr_set_voice_mode(WoprVoice m) {
    s_mode = m;
    if (m == WoprVoice::OFF) wopr_speech_stop();
}

void wopr_speech_shutdown() {
    if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
    if (s_loader) { SDL_WaitThread(s_loader, nullptr); s_loader = nullptr; }
    if (SDL_AtomicGet(&s_ready)) { tts_shutdown(); SDL_AtomicSet(&s_ready, 0); }
}
