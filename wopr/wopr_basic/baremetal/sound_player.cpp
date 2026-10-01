// The note player. See sound_player.hpp. Built with -mgeneral-regs-only:
// it runs inside the timer interrupt, whose entry stub saves only the
// general-purpose registers.
#include "sound_player.hpp"
#include "audio.hpp"
#include "hw.hpp"

namespace {

struct Note { uint32_t centi_hz, tone_us, gap_us; };
constexpr int QSIZE = 1024;
Note queue[QSIZE];
volatile int q_head, q_tail;              // head: next to play (IRQ), tail: next free (main)

bool ready, card;
bool playing;                             // a note (tone or its gap) is under way
Note cur;
uint32_t tone_left_us, gap_left_us;
uint32_t phase, phase_inc;                // square wave, 0.32 fixed point
constexpr uint32_t TICK_US = 1000000 / 240;
constexpr int RATE = 48000;

inline uint32_t irq_save() {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return (uint32_t)f;
}
inline void irq_restore(uint32_t f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

void speaker(uint32_t centi_hz) {
    if (!centi_hz) { outb(0x61, inb(0x61) & ~3); return; }
    uint32_t div = (uint32_t)(119318200ull / centi_hz);
    if (div < 1) div = 1;
    if (div > 65535) div = 65535;
    outb(0x43, 0xB6);                     // channel 2, square wave
    outb(0x42, div & 0xFF);
    outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);            // gate on, speaker on
}

// Move on to the next note if the current one is done. Returns false when
// there's nothing to play.
bool advance() {
    if (playing && (tone_left_us || gap_left_us)) return true;
    if (q_head == q_tail) {
        if (playing && !card) speaker(0);
        playing = false;
        return false;
    }
    cur = queue[q_head];
    q_head = (q_head + 1) % QSIZE;
    tone_left_us = cur.tone_us;
    gap_left_us = cur.gap_us;
    phase_inc = cur.centi_hz ? (uint32_t)(((uint64_t)cur.centi_hz << 32) / (RATE * 100ull)) : 0;
    playing = true;
    if (!card) speaker(cur.centi_hz && tone_left_us ? cur.centi_hz : 0);
    return true;
}

// PC speaker: time passes in whole timer ticks.
void speaker_tick() {
    uint32_t us = TICK_US;
    while (us && advance()) {
        if (tone_left_us) {
            uint32_t t = tone_left_us < us ? tone_left_us : us;
            tone_left_us -= t; us -= t;
            if (!tone_left_us) speaker(0);
        } else {
            uint32_t t = gap_left_us < us ? gap_left_us : us;
            gap_left_us -= t; us -= t;
        }
    }
}

// Sound card: render the square wave sample by sample.
int16_t buf[2048];
void card_tick() {
    int n = audio_frames_wanted(RATE / 240);
    if (n > 2048) n = 2048;
    constexpr uint32_t SAMPLE_US_X1000 = 1000000000u / RATE;   // 20833 ns per sample
    static uint32_t ns_carry;
    for (int i = 0; i < n; i++) {
        int16_t v = 0;
        if (advance()) {
            if (tone_left_us) {
                if (cur.centi_hz) v = (phase & 0x80000000u) ? -9000 : 9000;
                phase += phase_inc;
            }
            ns_carry += SAMPLE_US_X1000;
            uint32_t us = ns_carry / 1000;
            ns_carry %= 1000;
            if (tone_left_us) tone_left_us = tone_left_us > us ? tone_left_us - us : 0;
            else gap_left_us = gap_left_us > us ? gap_left_us - us : 0;
        }
        buf[i] = v;
    }
    audio_submit(buf, n);
}

} // namespace

void player_init(bool use_card) {
    card = use_card;
    q_head = q_tail = 0;
    playing = false;
    if (!card) speaker(0);
    ready = true;
}
bool player_on_card() { return card; }

bool player_push(uint32_t centi_hz, uint32_t tone_ms, uint32_t gap_ms) {
    uint32_t f = irq_save();
    int next = (q_tail + 1) % QSIZE;
    bool ok = next != q_head;
    if (ok) {
        queue[q_tail] = {centi_hz, tone_ms * 1000, gap_ms * 1000};
        q_tail = next;
    }
    irq_restore(f);
    return ok;
}

bool player_busy() {
    uint32_t f = irq_save();
    bool b = q_head != q_tail || (playing && (tone_left_us || gap_left_us));
    irq_restore(f);
    return b;
}

void player_stop() {
    uint32_t f = irq_save();
    q_head = q_tail;
    playing = false;
    tone_left_us = gap_left_us = 0;
    if (ready && !card) speaker(0);
    irq_restore(f);
}

void player_tick() {
    if (!ready) return;
    if (card) card_tick();
    else speaker_tick();
}
