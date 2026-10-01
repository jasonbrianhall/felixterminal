// The same cases through libc.cpp, as a static program with no C library.
#include <stdint.h>
#include <stddef.h>
#include "libc_test_cases.h"
extern "C" {
int snprintf(char*, size_t, const char*, ...);
size_t strlen(const char*);
double sin(double), cos(double), tan(double), atan(double), atan2(double, double), exp(double), log(double),
       log10(double), pow(double, double), sqrt(double), floor(double), ceil(double), fmod(double, double),
       hypot(double, double), strtod(const char*, char**), atof(const char*), asin(double), acos(double);
void serial_putc(char) {}
void serial_puts(const char*) {}
}
static void wr(const char* s) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(1), "S"(s), "d"(strlen(s)) : "rcx", "r11", "memory");
}
void console_write(const char*, size_t) {}
int console_getchar() { return 0; }
void platform_sleep_ms(uint32_t) {}
uint32_t platform_ms() { return 0; }
bool storage_resolve(const char*, char*, size_t) { return false; }
bool storage_ready() { return false; }
bool storage_write_protected() { return false; }
bool storage_disk_write_protected() { return false; }
const char* storage_cwd() { return "/"; }
void storage_set_cwd(const char*) {}
extern "C" void start_c() {
    char b[512];
#define X(f, v) snprintf(b, sizeof b, f, v); wr(f); wr("\t"); wr(b); wr("\n");
    CASES(X) ICASES(X)
#undef X
    snprintf(b, sizeof b, "%s|%-6s|%6.2s|", "hi", "ab", "xyz"); wr("s\t"); wr(b); wr("\n");
#define X(e) snprintf(b, sizeof b, "%-30s %.15g\n", #e, (double)(e)); wr(b);
    MCASES(X)
    __asm__ volatile("syscall" :: "a"(60), "D"(0));
}
__asm__(".global _start\n_start: andq $-16, %rsp\n call start_c\n");
