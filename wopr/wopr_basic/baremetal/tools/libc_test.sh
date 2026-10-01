#!/bin/sh
# Check libc.cpp's printf formatting, strtod and x87 math against glibc
# (x86_64 Linux host). Prints "identical" or the differences.
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
${CXX:-g++} -O2 -w libc_test_ref.cpp -o "$T/ref" -lm
${CXX:-g++} -O2 -std=gnu++17 -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -fno-math-errno \
    -U_FORTIFY_SOURCE -nostdlib -static -fno-pie -no-pie -I.. \
    libc_test_bm.cpp ../libc.cpp ../runtime.cpp ../fat12.cpp -o "$T/bm" $(${CXX:-g++} -print-libgcc-file-name)
"$T/ref" > "$T/ref.txt"
"$T/bm" > "$T/bm.txt"
diff "$T/ref.txt" "$T/bm.txt" && echo "identical ($(wc -l < "$T/ref.txt") cases)"
