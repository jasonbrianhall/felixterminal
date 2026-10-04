#pragma once
// The note player behind SOUND, PLAY and BEEP. Integer-only and driven by
// the 240 Hz timer interrupt, so music keeps time however busy the
// interpreter is. It plays through the PC speaker, or as a square wave
// through the sound card when there is one (audio.cpp).
#include <stdint.h>

void player_init(bool use_card);          // after audio_init(); false = PC speaker
bool player_on_card();
// Queue a note: frequency in hundredths of a hertz (0 = rest), how long it
// sounds and the silence after it. False when the queue is full.
bool player_push(uint32_t centi_hz, uint32_t tone_ms, uint32_t gap_ms);
bool player_busy();                       // something queued or playing
void player_stop();                       // silence now, drop the queue
void player_hold();                       // stop using the card until player_init (switching outputs)
void player_tick();                       // irq.cpp: every timer interrupt
