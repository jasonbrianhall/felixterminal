/* bigmem_dos.c: large blocks straight from the DPMI host, and back to it
   when they're freed.

   DJGPP's malloc takes memory from the DPMI host as the heap grows and never
   gives it back: a program's memory is the most it ever needed. The big
   blocks are pictures (CHIRP.EXE: the decoded pixels, their texture;
   FLT.EXE: sixel and kitty images, their canvas and texture), so the memory
   held was that of the largest picture shown, until the program exited.
   Both link this file (chirp/Makefile, Makefile).

   malloc, calloc, realloc and free are wrapped (-Wl,--wrap=..., see the
   Makefile), everywhere: the program, SDL, stb_image, libwebp, libpng, OpenSSL, C++'s new. A block
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

// Does the host split a freed block? Some keep a freed block for reuse and
// hand all of it to the next request it can hold (vmdos before its DPMI
// gave large freed blocks back): a freed 36 MB picture then went to a 1 MB
// texture, the next picture needed 36 MB more, and memory ran out browsing
// pictures. Asked: 1 MB taken and freed, then 256 KB taken; a host that
// charges the whole 1 MB for it (INT 31h 0500h's free pages) keeps freed
// blocks whole, and DJGPP's malloc (memory kept, but no more than the most
// needed at once) is better there.
static int host_frees_well(void) {
    __dpmi_free_mem_info fi;
    __dpmi_meminfo a, b;
    unsigned long f1, f2;
    a.size = 1024ul * 1024ul;
    if (__dpmi_allocate_memory(&a) != 0) return 0;
    __dpmi_free_memory(a.handle);
    if (__dpmi_get_free_memory_information(&fi) != 0) return 0;
    f1 = fi.total_number_of_free_pages;
    b.size = 256ul * 1024ul;
    if (__dpmi_allocate_memory(&b) != 0) return 0;
    int ok = __dpmi_get_free_memory_information(&fi) == 0;
    f2 = fi.total_number_of_free_pages;
    __dpmi_free_memory(b.handle);
    if (!ok || f1 == 0xFFFFFFFFul || f2 == 0xFFFFFFFFul) return 0;
    return f1 - f2 < 128;                          /* (256 KB is 64 pages; the whole 1 MB, 256) */
}

int g_bigmem_state;                                /* (CHIRP_LOG: 1 on, -1 off) */
static int big_ready(void) {
    if (!s_state) {
        const char *e = getenv("CHIRP_BIGMEM");      /* SET CHIRP_BIGMEM=0 (FLT_BIGMEM=0): DJGPP's malloc for everything */
        if (!e) e = getenv("FLT_BIGMEM");
        s_state = !(e && *e == '0') && host_frees_well() && __djgpp_nearptr_enable() ? 1 : -1;
    }
    g_bigmem_state = s_state;
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

/* Every block from DJGPP's malloc carries a small header too, with its size:
   realloc can then move one that grows to BIG or more out to a DPMI block
   of its own. (Without its size it had to stay in DJGPP's heap, which never
   shrinks: stb_image gathers a PNG's compressed data in a buffer that grows
   from 4 KB, so the largest PNG's few megabytes, and the holes its moves
   left, stayed taken until Chirp exited. Pictures of every size went that
   way, a step down in free memory at each one larger than any before.) */
#define SMAGIC 0x534D4C4Cu                 /* 'SMLL' */
struct small_header { uint32_t magic, size, check, pad; };
unsigned long g_heap_live, g_heap_peak;    /* (CHIRP_LOG: bytes in DJGPP's heap now, at most) */

static struct small_header *small_of(void *p) {
    if (!p) return 0;
    struct small_header *h = (struct small_header *)p - 1;
    if (h->magic != SMAGIC || h->check != (SMAGIC ^ h->size ^ (uint32_t)(uintptr_t)h)) return 0;
    return h;
}
static void *small_done(struct small_header *h, size_t n) {
    if (!h) return 0;
    h->magic = SMAGIC; h->size = (uint32_t)n; h->check = SMAGIC ^ h->size ^ (uint32_t)(uintptr_t)h; h->pad = 0;
    g_heap_live += n;
    if (g_heap_live > g_heap_peak) g_heap_peak = g_heap_live;
    return h + 1;
}
static void *small_alloc(size_t n) {
    if (n > (size_t)-1 - sizeof(struct small_header)) return 0;
    return small_done((struct small_header *)__real_malloc(n + sizeof(struct small_header)), n);
}
static void small_free(struct small_header *h) {
    g_heap_live -= h->size;
    h->magic = 0;
    __real_free(h);
}

void *__wrap_malloc(size_t n) {
    void *p = big_alloc(n);
    return p ? p : small_alloc(n);
}

void *__wrap_calloc(size_t k, size_t n) {
    if (n && k > (size_t)-1 / n) return 0;
    size_t t = k * n;
    void *p = __wrap_malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void __wrap_free(void *p) {
    struct big_header *h;
    struct small_header *sh;
    if (!p) return;
    if ((h = header_of(p))) big_free(h);
    else if ((sh = small_of(p))) small_free(sh);
    else __real_free(p);                   /* (one of DJGPP's own, as memalign's) */
}

void *__wrap_realloc(void *p, size_t n) {
    if (!p) return __wrap_malloc(n);
    if (!n) { __wrap_free(p); return 0; }
    struct big_header *h = header_of(p);
    struct small_header *sh = h ? 0 : small_of(p);
    if (!h && !sh) return __real_realloc(p, n);
    size_t old = h ? h->size : sh->size;
    if (h && n <= old && n >= old / 2) {   /* a big one, a little smaller: as it is */
        h->size = (uint32_t)n; h->check = MAGIC ^ h->handle ^ h->size ^ (uint32_t)(uintptr_t)h;
        return p;
    }
    if (sh && (n < BIG || s_state < 0)) {  /* a small one staying small: DJGPP's realloc */
        g_heap_live -= old;
        sh->magic = 0;
        struct small_header *q = (struct small_header *)__real_realloc(sh, n + sizeof *sh);
        if (!q) { small_done(sh, old); return 0; }
        return small_done(q, n);
    }
    void *q = __wrap_malloc(n);            /* (to a DPMI block when it's grown to BIG) */
    if (!q) return 0;
    memcpy(q, p, n < old ? n : old);
    __wrap_free(p);
    return q;
}
