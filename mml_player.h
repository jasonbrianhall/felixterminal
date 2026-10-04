#pragma once

// ============================================================================
// MML PLAYER
// QB/GW-BASIC compatible Music Macro Language player.
// Mixes into the existing SDL audio device via term_audio_get_device().
//
// Supported MML tokens:
//   A-G [#/+/-] [length]  Note (with optional sharp/flat and duration)
//   R / P [length]         Rest
//   O n                    Octave (0-7, default 4)
//   < / >                  Octave down / up
//   L n                    Default note length (1,2,4,8,16,32,64)
//   T n                    Tempo in BPM (32-255, default 120)
//   V n                    Volume (0-15, default 8)
//   N n                    Play note n (1-84 = O0 C .. O6 B, 0 = rest)
//   MN / ML / MS           Music Normal / Legato / Staccato (articulation)
//   MB / MF                Music Background / Foreground (ignored — always async)
//   . after length         Dotted note (x1.5 duration)
//
// Usage:
//   mml_init();                         // call once after term_audio_init()
//   mml_play("T120O4L8CDEFGAB");        // queue and start playing
//   mml_shutdown();                     // call before term_audio_shutdown()
// ============================================================================

// Call once after term_audio_init().
void mml_init(void);

// Parse and play an MML string. Non-blocking — audio runs in the SDL callback.
// Calling again while playing replaces the current sequence.
void mml_play(const char *mml);

// Stop playback immediately.
void mml_stop(void);

// Returns true if currently playing.
bool mml_is_playing(void);

// Call before term_audio_shutdown().
void mml_shutdown(void);

// Pre-render the MML string to a float buffer and play it via the WOPR
// audio channel. Non-blocking. Frees the previous buffer automatically.
void mml_play_via_wopr(const char *mml);
