// The expected output: glibc's printf and libm on the same cases.
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "libc_test_cases.h"
int main() {
    char b[512];
#define X(f, v) snprintf(b, sizeof b, f, v); printf("%s\t%s\n", f, b);
    CASES(X) ICASES(X)
#undef X
    snprintf(b, sizeof b, "%s|%-6s|%6.2s|", "hi", "ab", "xyz"); printf("s\t%s\n", b);
#define X(e) printf("%-30s %.15g\n", #e, (double)(e));
    MCASES(X)
}
