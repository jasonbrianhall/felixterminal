/* clock_dos.c: a smooth uclock() for Felix Chirp on DOS.

   DJGPP's uclock() (which SDL's DOS timer, SDL_GetTicks and SDL_Delay run
   on) adds the BIOS tick count at 0040:006C to the PIT's counter. Where the
   two don't move together (under vmdos, the counter wraps before the tick's
   IRQ 0 is counted, or the other way round), it jumps 55 ms ahead once a
   tick and reads back again after; SDL won't let time go back, so it stands
   still until it catches up: everything timed by it (the visualizer, from
   the song's position) stops for up to 55 ms, 18 times a second.

   This one counts the CPU's time-stamp counter (RDTSC), which goes on
   evenly, at a rate measured against the BIOS ticks: first over three ticks
   at the first call, then again at a tick edge (seen within 2 ms) every 2
   seconds, from the first edge, so each measure is better than the last;
   never with a jump: the time so far is kept and the new rate counts from
   there. Without a
   TSC (a 486), it's DJGPP's uclock() as it was.

   The Makefile links with -Wl,--wrap=uclock: every uclock() call, SDL's
   too, comes here, and __real_uclock is DJGPP's. */
#include <time.h>
#include <go32.h>
#include <sys/farptr.h>

uclock_t __real_uclock(void);

static unsigned long long rdtsc(void)
{
    unsigned long long v;
    __asm__ __volatile__("rdtsc" : "=A"(v));
    return v;
}

static unsigned long bios_ticks(void)
{
    return _farpeekl(_dos_ds, 0x46C);
}

static int have_tsc(void)
{
    unsigned long f0, f1, a, b, c, d;
    /* CPUID there? (the ID flag, bit 21 of EFLAGS, can be changed) */
    __asm__ __volatile__("pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl;"
                         "pushfl; popl %0; pushl %1; popfl" : "=&r"(f1), "=&r"(f0));
    if (!((f0 ^ f1) & 0x200000)) return 0;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (d >> 4) & 1;
}

#define TICK_SECONDS (65536.0 / 1193180.0)   /* a BIOS tick: 54.9 ms */

static int state;                            /* 0: not started, 1: TSC, -1: no TSC */
static unsigned long long tsc0;              /* at a tick edge: where the rate is measured from */
static unsigned long ticks0;
static unsigned long long base_tsc;          /* the time so far: base_out at base_tsc */
static double base_out;
static double per_tsc;                       /* uclock units a TSC count */
static unsigned long next_ticks;             /* measure the rate again at this tick */

static unsigned long wait_tick(void)         /* the next tick edge */
{
    unsigned long t = bios_ticks(), n;
    unsigned long long limit = 0;
    while ((n = bios_ticks()) == t) {
        if (++limit > 2000000000ULL) break;  /* (no ticks at all: give up) */
    }
    return n;
}

static void start(void)
{
    if (!have_tsc()) { state = -1; return; }
    unsigned long t0 = wait_tick();
    unsigned long long a = rdtsc();
    unsigned long t1 = t0;
    while (t1 - t0 < 3) t1 = wait_tick();    /* about 165 ms */
    unsigned long long b = rdtsc();
    if (b <= a || t1 == t0) { state = -1; return; }
    tsc0 = a; ticks0 = t0;
    per_tsc = (t1 - t0) * TICK_SECONDS * UCLOCKS_PER_SEC / (double)(b - a);
    base_tsc = b;
    base_out = 0;
    next_ticks = t1 + 37;                    /* 2 seconds on */
    state = 1;
}

uclock_t __wrap_uclock(void)
{
    static unsigned long long last_tsc;
    static unsigned long last_tick;
    if (state == 0) start();
    if (state < 0) return __real_uclock();
    unsigned long long now = rdtsc();
    unsigned long t = bios_ticks();
    /* At a tick edge (seen within 2 ms of the call before), 2 seconds or more
       on: measure the rate again, from the first edge to this one. */
    if (t != last_tick && t == last_tick + 1 && (double)(now - last_tsc) * per_tsc < UCLOCKS_PER_SEC / 500.0 &&
        (long)(t - next_ticks) >= 0) {
        double out_now = base_out + (double)(now - base_tsc) * per_tsc;
        double rate = (t - ticks0) * TICK_SECONDS * UCLOCKS_PER_SEC / (double)(now - tsc0);
        if (rate > per_tsc * 0.9 && rate < per_tsc * 1.1) per_tsc = rate;
        base_out = out_now;
        base_tsc = now;
        next_ticks = t + 37;
    }
    last_tsc = now;
    last_tick = t;
    return (uclock_t)(base_out + (double)(now - base_tsc) * per_tsc);
}
