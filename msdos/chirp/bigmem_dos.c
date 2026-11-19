/* bigmem_dos.c: large blocks straight from the DPMI host, and back to it
   when they're freed.

   DJGPP's malloc takes memory from the DPMI host as the heap grows and never
   gives it back: a program's memory is the most it ever needed. Chirp's big
   blocks are pictures (the decoded pixels, their texture), so the memory it
   held was that of the largest picture it had opened until it exited.

   malloc, calloc, realloc and free are wrapped (-Wl,--wrap=..., see the
   Makefile), everywhere: Chirp, SDL, stb_image, libwebp, C++'s new. A block
   of BIG bytes or more is a DPMI memory block of its own (INT 31h 0501h),
   freed to the host at once (0502h); anything smaller is DJGPP's malloc.
   A DPMI block is outside DJGPP's data segment's usual limit, so this needs
   near pointers (__djgpp_nearptr_enable); where the host refuses them,
   everything is DJGPP's malloc, as before.

   Each big block starts with a header (in front of the pointer handed out)
   saying it is one: free and realloc look at the 16 bytes before a pointer,
   which for DJGPP's own blocks are its heap (always there to read). */

#include <dpmi.h>
#include <sys/nearptr.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void  __real_free(void *);

#define BIG      (256u * 1024u)
#define MAGIC    0x4249474Du               /* 'BIGM' */

struct big_header {                        /* 16 bytes: the block's pointer stays 16-aligned */
    uint32_t magic;
    uint32_t handle;                       /* the DPMI block's handle */
    uint32_t size;                         /* bytes asked for */
    uint32_t check;                        /* magic ^ handle ^ size ^ where it is */
};

static int s_state;                        /* 0: not tried yet, 1: near pointers on, -1: refused */
unsigned long g_bigmem_blocks, g_bigmem_bytes;   /* (now held: CHIRP_LOG shows them) */

static int big_ready(void) {
    if (!s_state) s_state = __djgpp_nearptr_enable() ? 1 : -1;
    return s_state > 0;
}

static struct big_header *header_of(void *p) {
    if (!p || s_state <= 0) return 0;
    struct big_header *h = (struct big_header *)p - 1;
    if (h->magic != MAGIC || h->check != (MAGIC ^ h->handle ^ h->size ^ (uint32_t)(uintptr_t)h)) return 0;
    return h;
}

static void *big_alloc(size_t n) {
    if (n < BIG || n > 0x7FFFFFF0u || !big_ready()) return 0;
    __dpmi_meminfo mi;
    mi.size = (unsigned long)(n + sizeof(struct big_header) + 4095) & ~4095ul;
    if (__dpmi_allocate_memory(&mi) != 0) return 0;
    struct big_header *h = (struct big_header *)(mi.address + __djgpp_conventional_base);
    h->magic = MAGIC; h->handle = mi.handle; h->size = (uint32_t)n;
    h->check = MAGIC ^ h->handle ^ h->size ^ (uint32_t)(uintptr_t)h;
    g_bigmem_blocks++; g_bigmem_bytes += mi.size;
    return h + 1;
}

static void big_free(struct big_header *h) {
    uint32_t handle = h->handle;
    g_bigmem_blocks--; g_bigmem_bytes -= (h->size + sizeof *h + 4095) & ~4095ul;
    h->magic = 0;
    __dpmi_free_memory(handle);
}

void *__wrap_malloc(size_t n) {
    void *p = big_alloc(n);
    return p ? p : __real_malloc(n);
}

void *__wrap_calloc(size_t k, size_t n) {
    if (n && k > (size_t)-1 / n) return 0;
    size_t t = k * n;
    void *p = big_alloc(t);
    if (p) { memset(p, 0, t); return p; }   /* (a DPMI block isn't always zeroed) */
    return __real_calloc(k, n);
}

void __wrap_free(void *p) {
    struct big_header *h = header_of(p);
    if (h) big_free(h);
    else __real_free(p);
}

void *__wrap_realloc(void *p, size_t n) {
    struct big_header *h = header_of(p);
    if (!p) return __wrap_malloc(n);
    if (!n) { __wrap_free(p); return 0; }
    size_t old = h ? h->size : 0;
    if (!h) return __real_realloc(p, n);   /* (DJGPP's malloc doesn't say how big its block is,
                                               so one of its blocks stays one of its blocks) */
    if (n <= old && n >= old / 2) { h->size = (uint32_t)n; h->check = MAGIC ^ h->handle ^ h->size ^ (uint32_t)(uintptr_t)h; return p; }
    void *q = __wrap_malloc(n);
    if (!q) return 0;
    memcpy(q, p, n < old ? n : old);
    big_free(h);
    return q;
}
