// The note player (baremetal/sound_player.hpp) for DOS: notes from SOUND,
// PLAY and BEEP (baremetal/sound_bm.cpp) through the PC speaker. There's no
// timer interrupt of our own; player_tick() runs whenever the interpreter
// reads the clock or polls the keyboard, which it does between statements.
#include "sound_player.hpp"
#include <pc.h>
#include <time.h>

namespace {
struct Note { uint32_t centi_hz, tone_us, gap_us; };
constexpr int QSIZE = 1024;
Note queue[QSIZE];
int q_head, q_tail;
bool playing, in_gap;
Note cur;
uint64_t phase_end;                       // when the current tone (or gap) ends, us

uint64_t now_us() { return (uint64_t)uclock() * 1000000ull / UCLOCKS_PER_SEC; }

void speaker(uint32_t centi_hz) {
    if (!centi_hz) { outportb(0x61, inportb(0x61) & ~3); return; }
    uint32_t div = (uint32_t)(119318200ull / centi_hz);
    if (div < 1) div = 1;
    if (div > 65535) div = 65535;
    outportb(0x43, 0xB6);
    outportb(0x42, div & 0xFF);
    outportb(0x42, div >> 8);
    outportb(0x61, inportb(0x61) | 3);
}
}

void player_init(bool) { q_head = q_tail = 0; playing = false; speaker(0); }
bool player_on_card() { return false; }
void player_hold() {}

bool player_push(uint32_t centi_hz, uint32_t tone_ms, uint32_t gap_ms) {
    int next = (q_tail + 1) % QSIZE;
    if (next == q_head) { player_tick(); return false; }
    queue[q_tail] = {centi_hz, tone_ms * 1000, gap_ms * 1000};
    q_tail = next;
    player_tick();
    return true;
}

bool player_active() { return playing || q_head != q_tail; }

bool player_busy() { player_tick(); return playing || q_head != q_tail; }

void player_stop() { q_head = q_tail; playing = false; speaker(0); }

void player_tick() {
    uint64_t t = now_us();
    for (;;) {
        if (playing) {
            if (t < phase_end) return;
            if (!in_gap && cur.gap_us) {
                speaker(0);
                in_gap = true;
                phase_end += cur.gap_us;
                continue;
            }
            playing = false;
        }
        if (q_head == q_tail) { speaker(0); return; }
        cur = queue[q_head];
        q_head = (q_head + 1) % QSIZE;
        playing = true;
        in_gap = false;
        // Keep time from the end of the last note, unless we fell far behind.
        uint64_t start = phase_end && t - phase_end < 100000 ? phase_end : t;
        phase_end = start + cur.tone_us;
        speaker(cur.tone_us ? cur.centi_hz : 0);
    }
}
