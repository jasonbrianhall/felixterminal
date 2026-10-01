// sound.h for bare metal: SOUND, BEEP and PLAY (Music Macro Language) feed
// the note player (sound_player.cpp), which plays them through the PC
// speaker or the sound card from the timer interrupt. The MML parser follows
// ../sound_sdl.cpp.
#include "basic_ns.h"
#include "sound.h"
#include "sound_player.hpp"
#include <math.h>
#include <ctype.h>
#include <SDL2/SDL.h>

BASIC_NS_BEGIN

void sound_init(void) {}                  // the kernel set the player up at boot
void sound_shutdown(void) { sound_drain(); }

void sound_drain(void) {
    while (player_busy()) SDL_Delay(5);
}

void sound_stop(void) { player_stop(); }

static void q_push(double freq, int tone_ms, int gap_ms) {
    if (freq < 0) freq = 0;
    if (freq > 32767) freq = 32767;
    uint32_t centi = freq >= 1.0 ? (uint32_t)(freq * 100.0 + 0.5) : 0;
    while (!player_push(centi, tone_ms < 0 ? 0 : tone_ms, gap_ms < 0 ? 0 : gap_ms))
        SDL_Delay(5);                     // queue full: wait for room
}

void sound_beep(void) {
    sound_tone(800.0, 4.55);              // ~0.25 s, as on the IBM PC
    sound_drain();
}

// SOUND freq, duration in clock ticks (18.2 per second).
void sound_tone(double freq, double duration_ticks) {
    int ms = (int)(duration_ticks / 18.2 * 1000.0);
    if (ms < 1) ms = 1;
    q_push(freq, ms, 0);
}

static const int note_semitone[7] = {9, 11, 0, 2, 4, 5, 7};   // A..G from C
static double midi_to_freq(int midi) { return 440.0 * pow(2.0, (midi - 69) / 12.0); }
static int note_index(char c) {
    c = (char)toupper((unsigned char)c);
    return c >= 'A' && c <= 'G' ? c - 'A' : -1;
}

void sound_play(char* mml) {
    if (!mml) return;
    int octave = 4, length = 4, tempo = 120, foreground = 0;
    double tone_frac = 7.0 / 8.0;          // MN
    char* p = mml;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (!*p) break;
        char cmd = (char)toupper((unsigned char)*p++);
        if (cmd == 'O') {
            if (isdigit((unsigned char)*p)) { octave = *p++ - '0'; if (octave > 6) octave = 6; }
        } else if (cmd == '>') {
            if (octave < 6) octave++;
        } else if (cmd == '<') {
            if (octave > 0) octave--;
        } else if (cmd == 'L') {
            int n = 0;
            while (isdigit((unsigned char)*p)) n = n * 10 + (*p++ - '0');
            if (n >= 1 && n <= 64) length = n;
        } else if (cmd == 'T') {
            int n = 0;
            while (isdigit((unsigned char)*p)) n = n * 10 + (*p++ - '0');
            if (n >= 32 && n <= 255) tempo = n;
        } else if (cmd == 'M') {
            char sub = (char)toupper((unsigned char)*p);
            if (sub == 'N') { tone_frac = 7.0 / 8.0; p++; }
            else if (sub == 'L') { tone_frac = 1.0; p++; }
            else if (sub == 'S') { tone_frac = 3.0 / 4.0; p++; }
            else if (sub == 'F') { foreground = 1; p++; }
            else if (sub == 'B') { foreground = 0; p++; }
        } else if (cmd == 'P') {
            int dur = 0;
            while (isdigit((unsigned char)*p)) dur = dur * 10 + (*p++ - '0');
            if (dur < 1 || dur > 64) dur = length;
            double ms = (60000.0 / tempo) * (4.0 / dur);
            if (*p == '.') { ms *= 1.5; p++; }
            q_push(0.0, (int)ms, 0);
        } else if (cmd == 'N') {
            int n = 0;
            while (isdigit((unsigned char)*p)) n = n * 10 + (*p++ - '0');
            double ms = (60000.0 / tempo) * (4.0 / length);
            if (*p == '.') { ms *= 1.5; p++; }
            if (n == 0) q_push(0.0, (int)ms, 0);
            else q_push(midi_to_freq(n), (int)(ms * tone_frac), (int)(ms * (1.0 - tone_frac)));
        } else {
            int ni = note_index(cmd);
            if (ni < 0) continue;
            int semitone = note_semitone[ni];
            if (*p == '#' || *p == '+') { semitone++; p++; }
            else if (*p == '-') { semitone--; p++; }
            int dur = 0;
            while (isdigit((unsigned char)*p)) dur = dur * 10 + (*p++ - '0');
            if (dur < 1 || dur > 64) dur = length;
            double ms = (60000.0 / tempo) * (4.0 / dur);
            if (*p == '.') { ms *= 1.5; p++; }
            int midi = (octave + 1) * 12 + semitone;
            if (midi < 0) midi = 0;
            if (midi > 127) midi = 127;
            int tone_ms = (int)(ms * tone_frac);
            if (tone_ms < 1) tone_ms = 1;
            q_push(midi_to_freq(midi), tone_ms, (int)(ms * (1.0 - tone_frac)));
        }
    }
    if (foreground) sound_drain();
}

BASIC_NS_END
