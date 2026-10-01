// Interrupt handlers. Built with -mgeneral-regs-only so they never touch
// SSE registers (the assembly stubs only save general-purpose registers).
#include <stdint.h>
#include "hw.hpp"
#include "sound_player.hpp"

namespace StandaloneBasic { extern volatile int g_break; }   // ../main.cpp
// The PIT runs at 240 Hz: `fine_ticks` counts every interrupt (sound is
// topped up that often), `ticks` every 4th (60 Hz, the game's frame clock).
volatile uint32_t ticks, fine_ticks;
volatile uint8_t kbd_buf[256];
volatile uint8_t kbd_head, kbd_tail;

extern "C" void irq_timer() {
    fine_ticks = fine_ticks + 1;
    if ((fine_ticks & 3) == 0) ticks = ticks + 1;
    outb(0x20, 0x20);
    player_tick();                            // SOUND / PLAY keep time even mid-statement
}

// Ctrl+C and Ctrl+Break stop a running program right here, so even a loop
// that never looks at the keyboard can be broken into.
extern "C" void irq_keyboard() {
    static bool e0, ctrl;
    uint8_t b = inb(0x60);
    if (b == 0xE0) e0 = true;
    else {
        uint8_t code = b & 0x7F;
        bool down = !(b & 0x80);
        if (code == 0x1D) ctrl = down;
        else if (down && ((ctrl && !e0 && code == 0x2E) || (e0 && code == 0x46))) StandaloneBasic::g_break = 1;
        e0 = false;
    }
    kbd_buf[kbd_head] = b;
    kbd_head = kbd_head + 1;
    outb(0x20, 0x20);
}

extern "C" void irq_spurious() {}
