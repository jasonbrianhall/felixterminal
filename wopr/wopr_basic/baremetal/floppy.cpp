// Floppy controller driver. See floppy.hpp.
#include "floppy.hpp"
#include "hw.hpp"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern volatile uint32_t ticks;          // irq.cpp: the PIT at 60 Hz
void gfx_sdl_render(void);               // basic_gfx_sdl.cpp: show the screen now

namespace {

enum : uint16_t { DOR = 0x3F2, MSR = 0x3F4, FIFO = 0x3F5, DIR = 0x3F7, CCR = 0x3F7 };
enum : uint8_t { RQM = 0x80, DIO = 0x40, CB = 0x10 };
enum : uint8_t { DOR_DMA = 0x08, DOR_RUN = 0x04, DOR_MOTOR_A = 0x10 };
enum : uint8_t {
    CMD_SPECIFY = 0x03, CMD_RECALIBRATE = 0x07, CMD_SENSE_INT = 0x08, CMD_SEEK = 0x0F,
    CMD_READ = 0x66,     // READ DATA, MFM, skip deleted sectors
    CMD_WRITE = 0x45,    // WRITE DATA, MFM
};

// One track's worth of DMA buffer. ISA DMA reaches only the first 16 MiB and
// can't cross a 64 KiB boundary; the alignment takes care of the second.
alignas(65536) uint8_t dma_buf[36 * 512];

bool present;
uint32_t spt = 18, heads = 2, total = 2880;
uint8_t rate;                // CCR data rate: 0 = 500 kbit/s, 2 = 250, 3 = 1 Mbit/s
int cur_cyl = -1;
// Track cache: whole sides of tracks, a few of them, least recently used
// out. Reading a whole side costs about what one sector does (the wait is
// for the disk to come round), and FAT work reads the same neighbouring
// sectors over and over -- DIR looks every file up again, and the root
// directory straddles two sides.
struct Side { int c = -1, h = -1; uint32_t used = 0; uint8_t data[36 * 512]; };
Side sides[4];
uint32_t use_clock;
Side* cached(uint32_t c, uint32_t h) {
    for (Side& sd : sides)
        if (sd.c == (int)c && sd.h == (int)h) { sd.used = ++use_clock; return &sd; }
    return nullptr;
}
void uncache() { for (Side& sd : sides) sd.c = sd.h = -1; }
bool motor, wprot;
uint32_t motor_off_at;       // tick to stop the motor (0: not scheduled)
bool quiet;                  // probing the data rate: failures are expected
bool started;                // floppy_init done: the screen can be refreshed
char why[96];                // what the last failed transfer ran into

bool trace;                  // floppy=trace: report every request
bool stall_seen;             // the timer stopped: every wait gives up at once

// Milliseconds, in 1/60 s steps.
uint32_t now() { return ticks * 50 / 3; }

// Every wait below is timed by the timer interrupt. If the timer stops
// (interrupts left off), a wait would never end and the machine would just
// hang. The PIT itself keeps counting regardless, wrapping 240 times a
// second (irq.cpp): two seconds' worth of wraps with no tick, and we say so
// and give up.
void report(const char* fmt, ...);
uintptr_t irq_off();
void irq_restore(uintptr_t f);
bool stalled() {
    static uint32_t last_ticks, wraps;
    static uint16_t last_cnt;
    if (stall_seen) return true;
    if (ticks != last_ticks) { last_ticks = ticks; wraps = 0; return false; }
    uintptr_t f = irq_off();
    outb(0x43, 0x00);                                    // latch channel 0
    uint16_t cnt = inb(0x40);
    cnt |= (uint16_t)(inb(0x40) << 8);
    irq_restore(f);
    if (cnt > last_cnt) wraps++;                         // counted down to the bottom and reloaded
    last_cnt = cnt;
    if (wraps < 480) return false;
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0" : "=r"(fl));
    outb(0x20, 0x0A);
    uint8_t irr = inb(0x20);
    stall_seen = true;
    report("Floppy: the timer has stopped (interrupts %s, PIC mask %02X/%02X, IRR %02X)",
           (fl & 0x200) ? "on" : "off", inb(0x21), inb(0xA1), irr);
    return true;
}
bool waiting(uint32_t t, uint32_t ms) { return now() - t < ms && !stalled(); }

void delay_ms(uint32_t ms) {
    if (ms < 20) { for (uint32_t i = 0; i < ms * 1000; i++) outb(0x80, 0); return; }   // ~1 us each
    uint32_t t = now();
    while (waiting(t, ms)) __asm__ volatile("pause");
}

void report(const char* fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    if (started) gfx_sdl_render();
}

bool send(uint8_t b) {
    for (uint32_t t = now(); waiting(t, 500);)
        if ((inb(MSR) & (RQM | DIO)) == RQM) { outb(FIFO, b); return true; }
    return false;
}
bool recv(uint8_t& b) {
    for (uint32_t t = now(); waiting(t, 500);)
        if ((inb(MSR) & (RQM | DIO)) == (RQM | DIO)) { b = inb(FIFO); return true; }
    return false;
}
// SENSE INTERRUPT STATUS. Returns false if nothing is pending (ST0 0x80).
bool sense(uint8_t& st0, uint8_t& pcn) {
    if (!send(CMD_SENSE_INT) || !recv(st0)) return false;
    if (st0 == 0x80) return false;
    return recv(pcn);
}
// After SEEK or RECALIBRATE: poll until the drive reports seek end.
bool wait_seek(int cyl) {
    for (uint32_t t = now(); waiting(t, 3000); delay_ms(1)) {
        uint8_t st0, pcn;
        if (!sense(st0, pcn)) continue;
        if (!(st0 & 0x20)) continue;                     // not our seek-end yet
        return (st0 & 0xC0) == 0 && pcn == cyl;
    }
    return false;
}

uint8_t dor() { return DOR_DMA | DOR_RUN | (motor ? DOR_MOTOR_A : 0); }
void motor_on() {
    motor_off_at = 0;
    if (motor) return;
    motor = true;
    outb(DOR, dor());
    delay_ms(500);                                       // spin-up
}

bool reset() {
    uncache();
    outb(DOR, 0);
    for (int i = 0; i < 20; i++) outb(0x80, 0);          // > 4 us in reset (not by reading MSR:
                                                         // QEMU leaves reset on that, without the interrupt)
    outb(DOR, dor());
    // Let the controller finish coming out of reset before asking about
    // it. A SENSE INTERRUPT sent first only gets "invalid command", and on
    // PCem it also cancels the reset's own interrupt, so none of the four
    // ever turn up (the floppy then looks absent). Real 82077s want the
    // wait too.
    delay_ms(10);
    int got = 0;                                         // one interrupt per drive (4)
    for (uint32_t t = now(); got < 4 && waiting(t, 500);) {
        uint8_t st0, pcn;
        if (sense(st0, pcn)) got++; else delay_ms(1);
    }
    if (!got) return false;
    outb(CCR, rate);
    cur_cyl = -1;
    return send(CMD_SPECIFY) && send(0xDF) && send(0x02);   // step 3 ms, unload 240 ms; load 4 ms, DMA
}
bool recalibrate() {
    motor_on();
    for (int i = 0; i < 2; i++) {                        // one pass steps at most 77 tracks
        if (!send(CMD_RECALIBRATE) || !send(0)) return false;
        if (wait_seek(0)) { cur_cyl = 0; return true; }
    }
    cur_cyl = -1;
    return false;
}
bool seek(int c) {
    if (c == cur_cyl) return true;
    if (!send(CMD_SEEK) || !send(0) || !send((uint8_t)c)) return false;
    if (wait_seek(c)) { cur_cyl = c; return true; }
    cur_cyl = -1;
    return false;
}

// The DMA controller's byte flip-flop is shared by every channel, and the
// Sound Blaster code resets it and reads its own count from the timer
// interrupt (audio.cpp, sb_play_pos). Landing between our two byte
// accesses, it would put the second byte in the wrong half of an address
// or count. So no interrupts while we touch those registers.
uintptr_t irq_off() {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
void irq_restore(uintptr_t f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

void dma_setup(bool to_disk, uint32_t len) {
    uintptr_t f = irq_off();
    uint32_t a = (uint32_t)(uintptr_t)dma_buf;
    outb(0x0A, 0x06);                                    // mask channel 2
    outb(0x0C, 0xFF); outb(0x04, a & 0xFF); outb(0x04, (a >> 8) & 0xFF);
    outb(0x81, (a >> 16) & 0xFF);
    outb(0x0C, 0xFF); outb(0x05, (len - 1) & 0xFF); outb(0x05, (len - 1) >> 8);
    outb(0x0B, to_disk ? 0x4A : 0x46);                   // single mode, memory->disk or disk->memory
    (void)inb(0x08);                                     // clear any old terminal-count bits
    outb(0x0A, 0x02);                                    // unmask
    irq_restore(f);
}
// Channel 2's count register: len - 1 until the first byte moves, 0xFFFF
// once the last one has (terminal count).
uint16_t dma_count() {
    uintptr_t f = irq_off();
    outb(0x0C, 0xFF);
    uint8_t lo = inb(0x05), hi = inb(0x05);
    irq_restore(f);
    return (uint16_t)(lo | hi << 8);
}
bool transfer(bool to_disk, uint32_t c, uint32_t h, uint32_t s, uint32_t n) {
    dma_setup(to_disk, n * 512);
    const uint8_t cmd[9] = {to_disk ? CMD_WRITE : CMD_READ, (uint8_t)(h << 2), (uint8_t)c, (uint8_t)h,
                            (uint8_t)s, 2, (uint8_t)spt, 0x1B, 0xFF};
    for (uint8_t b : cmd)
        if (!send(b)) { snprintf(why, sizeof why, "controller not taking commands (MSR %02X)", inb(MSR)); return false; }
    // The DMA terminal count ends the command; wait for its result phase.
    // MSR alone can't be trusted for that: PCem shows RQM|DIO|CB after
    // every byte it moves by DMA (a real 82077 keeps RQM clear), so it
    // looked finished after the first byte and the buffer was copied out
    // half-filled. So: wait for the DMA controller's terminal count
    // (status register bit 2 for channel 2), or, if no byte has moved at
    // all, take RQM|DIO|CB as an error ending the command early. The count
    // register reaching 0xFFFF is terminal count too, should the status bit
    // be missed.
    uint32_t t = now(), len = n * 512;
    for (;;) {
        uint16_t left = dma_count();
        if ((inb(0x08) & 0x04) || left == 0xFFFF) { delay_ms(1); break; }   // then the results
        if ((inb(MSR) & (RQM | DIO | CB)) == (RQM | DIO | CB) && left == (uint16_t)(len - 1))
            break;
        if (!waiting(t, 3000)) {
            snprintf(why, sizeof why, "transfer never finished (MSR %02X, %u of %u bytes moved)",
                     inb(MSR), (unsigned)((uint16_t)(len - 1) - left), (unsigned)len);
            return false;
        }
    }
    uint8_t r[7];
    for (uint8_t& b : r)
        if (!recv(b)) { snprintf(why, sizeof why, "no result from the controller (MSR %02X)", inb(MSR)); return false; }
    if ((r[0] & 0xC0) == 0) return true;
    snprintf(why, sizeof why, "error ST0 %02X ST1 %02X ST2 %02X", r[0], r[1], r[2]);
    if (r[1] & 0x02) wprot = true;                       // ST1: not writable
    return false;
}

bool xfer(bool to_disk, uint32_t lba, uint32_t n, uint8_t* buf) {
    if (!present || lba + n > total) return false;
    wprot = false;
    motor_on();
    while (n) {
        uint32_t c = lba / (spt * heads), h = (lba / spt) % heads, s = lba % spt + 1;
        uint32_t cnt = spt - (s - 1);
        if (cnt > n) cnt = n;
        if (!to_disk && !quiet) {
            Side* sd = cached(c, h);
            if (!sd) {
                // The whole side, in one go, one attempt: if anything on it
                // won't read, fall back to just the sectors asked for.
                if (trace) report("Floppy: read C%u H%u (whole side, for S%u x%u)", c, h, s, cnt);
                if (seek((int)c) && transfer(false, c, h, 1, spt)) {
                    sd = &sides[0];
                    for (Side& o : sides) if (o.used < sd->used) sd = &o;
                    sd->c = (int)c; sd->h = (int)h; sd->used = ++use_clock;
                    memcpy(sd->data, dma_buf, spt * 512);
                }
            }
            if (sd) {
                memcpy(buf, sd->data + (s - 1) * 512, cnt * 512);
                lba += cnt; buf += cnt * 512; n -= cnt;
                continue;
            }
        }
        if (trace) report("Floppy: %s C%u H%u S%u x%u", to_disk ? "write" : "read", c, h, s, cnt);
        if (to_disk) memcpy(dma_buf, buf, cnt * 512);
        bool ok = false;
        int tries = quiet ? 2 : 5;                       // probing: don't linger on a wrong rate
        for (int attempt = 0; attempt < tries && !ok && !wprot; attempt++) {
            if (attempt == 3) reset();
            if (attempt) recalibrate();
            why[0] = 0;
            if (!seek((int)c)) snprintf(why, sizeof why, "seek to track %u failed", c);
            else ok = transfer(to_disk, c, h, s, cnt);
            if (!ok && !quiet)
                report("Floppy: %s C%u H%u S%u x%u, try %d: %s", to_disk ? "write" : "read",
                       c, h, s, cnt, attempt + 1, why[0] ? why : "?");
        }
        if (!ok) {
            uncache();                                   // what's on the disk is anyone's guess now
            if (quiet) return false;
            printf("Floppy: %s error at sector %u%s\n", to_disk ? "write" : "read", lba,
                   wprot ? " (write-protected)" : "");
            return false;
        }
        if (!to_disk) memcpy(buf, dma_buf, cnt * 512);
        else if (Side* sd = cached(c, h)) memcpy(sd->data + (s - 1) * 512, buf, cnt * 512);   // keep it current
        lba += cnt; buf += cnt * 512; n -= cnt;
    }
    return true;
}

uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

} // namespace

bool floppy_init() {
    if ((uintptr_t)dma_buf + sizeof dma_buf > 0x1000000) return false;   // ISA DMA can't reach it
    // CMOS register 0x10, high nibble: drive A: type.
    switch (cmos(0x10) >> 4) {
    case 2: rate = 0; spt = 15; break;                   // 5.25" 1.2 MB
    case 3: rate = 2; spt = 9;  break;                   // 3.5" 720 KB
    case 4: rate = 0; spt = 18; break;                   // 3.5" 1.44 MB
    case 5: rate = 3; spt = 36; break;                   // 3.5" 2.88 MB
    default:
        // CMOS says no drive, yet we were booted from A: (only then are we
        // called). Emulators leave the type to the BIOS setup, which may
        // never have been saved: assume the usual 3.5" 1.44 MB drive.
        printf("Floppy: CMOS has no drive A: type; assuming 1.44 MB\n");
        rate = 0; spt = 18; break;
    }
    heads = 2; total = spt * heads * 80;
    if (inb(MSR) == 0xFF || !reset() || !recalibrate()) {
        printf("Floppy: no controller\n");
        motor = false; outb(DOR, dor());
        return false;
    }
    present = true;
    // Geometry from the boot sector (sector 1 of track 0 is the same on all formats).
    // The disk needn't match the drive (a 1.44 MB disk in a 2.88 MB drive,
    // or a CMOS type that's simply wrong, as on a fresh PCem machine): try
    // the drive's own data rate first, then the others, until the boot
    // sector reads.
    uint8_t b[512];
    static const uint8_t rates[] = {0, 2, 3, 1};         // 1.44/1.2 MB, 720 KB, 2.88 MB, 300 kbit/s
    uint8_t first = rate;
    quiet = true;
    bool ok = xfer(false, 0, 1, b);
    for (uint8_t r : rates) {
        if (ok) break;
        if (r == first) continue;
        rate = r;
        outb(CCR, rate);
        ok = xfer(false, 0, 1, b);
    }
    quiet = false;
    if (!ok) { printf("Floppy: can't read the disk\n"); present = false; floppy_idle(); return false; }
    if (rate != first) printf("Floppy: disk read at a different data rate than the drive type suggests\n");
    uint32_t bspt = b[24] | b[25] << 8, bh = b[26] | b[27] << 8;
    uint32_t tot = b[19] | b[20] << 8;
    if (!tot) tot = b[32] | b[33] << 8 | b[34] << 16 | (uint32_t)b[35] << 24;
    if (bspt >= 8 && bspt <= 36 && (bh == 1 || bh == 2) && tot && tot <= bspt * bh * 84) {
        spt = bspt; heads = bh; total = tot;
    }
    floppy_check_media();                                // clear the change line
    printf("Floppy: drive A: %u sectors (%u/track, %u heads)\n", total, spt, heads);
    floppy_idle();
    started = true;
    return true;
}

bool floppy_read(uint32_t lba, uint32_t count, void* buf) { return xfer(false, lba, count, (uint8_t*)buf); }
bool floppy_write(uint32_t lba, uint32_t count, const void* buf) { return xfer(true, lba, count, (uint8_t*)buf); }
bool floppy_write_protected() { return wprot; }

bool floppy_disk_write_protected() {
    uint8_t st3 = 0;
    if (!present || !send(0x04) || !send(0) || !recv(st3)) return false;   // SENSE DRIVE STATUS
    return st3 & 0x40;
}

void floppy_set_trace(bool on) { trace = on; }

int floppy_check_media() {
    if (!present) return -1;
    if (trace) report("Floppy: check media (change line %s)", (inb(DIR) & 0x80) ? "set" : "clear");
    motor_on();
    if (!(inb(DIR) & 0x80)) return 0;
    uncache();                                           // a different disk, perhaps
    // The change line clears on a step once a disk is in the drive.
    cur_cyl = -1;
    seek(1);
    seek(0);
    if (cur_cyl != 0) recalibrate();
    return (inb(DIR) & 0x80) ? -1 : 1;
}

void floppy_idle() { motor_off_at = (now() + 2000) | 1; }
void floppy_poll() {
    if (motor && motor_off_at && (int32_t)(now() - motor_off_at) >= 0) {
        motor = false;
        motor_off_at = 0;
        outb(DOR, dor());
    }
}
