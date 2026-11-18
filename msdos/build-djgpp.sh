#!/bin/sh
# Build the DJGPP cross compiler FLT.EXE is made with: gcc 14.2.0 (C and
# C++), binutils 2.42 and DJGPP's C library from jwt27's djgpp-cvs, for
# i586-pc-msdosdjgpp, with jwt27's build-gcc scripts, at the commits FLT.EXE
# was built and tested with. Takes about an hour on two cores.
#
#   sh build-djgpp.sh              into ~/djgpp
#   PREFIX=/opt/djgpp sh build-djgpp.sh
#
# The Makefile finds the compiler in ~/djgpp, /opt/djgpp or /usr/local/djgpp
# (or give DJGPP=dir, or put dir/bin on the PATH).
#
# Needs (build-gcc's list):
#   Debian/Ubuntu: sudo apt install bison flex curl gcc g++ make texinfo zlib1g-dev
#                  tar bzip2 gzip xz-utils unzip python3-dev m4 dos2unix nasm git patch
#   Fedora:        sudo dnf install gcc-c++ which bison flex texinfo patch zlib-devel
#                  tar bzip2 gzip xz unzip python3-devel m4 dos2unix nasm git
# build-gcc downloads binutils, gcc, GMP, MPFR, MPC and ISL from GNU's mirrors.
set -e
PREFIX=${PREFIX:-$HOME/djgpp}
WORK=${WORK:-$(pwd)/build-djgpp}
BUILD_GCC_REV=9a41eab58311b0e5459757f2f0a98d2733e37a26    # jwt27/build-gcc, 2026-04-26
DJGPP_CVS_REV=ca9f7dcd7a0a02d16b688f88d8b65d7dea93f69a    # jwt27/djgpp-cvs (jwt27 branch), 2025-11-16

mkdir -p "$WORK" "$PREFIX"
cd "$WORK"
if [ ! -d build-gcc ]; then
    git clone -q https://github.com/jwt27/build-gcc
    git -C build-gcc -c advice.detachedHead=false checkout -q $BUILD_GCC_REV
fi
# DJGPP's C library at its commit: cloned where build-gcc looks for it, and
# --no-git-reset keeps build-gcc from moving it to the branch's newest commit.
if [ ! -d build-gcc/download/djgpp-cvs ]; then
    mkdir -p build-gcc/download
    git clone -q --branch jwt27 https://github.com/jwt27/djgpp-cvs.git build-gcc/download/djgpp-cvs
    git -C build-gcc/download/djgpp-cvs -c advice.detachedHead=false checkout -q $DJGPP_CVS_REV
fi

cd build-gcc
./build-djgpp.sh --prefix="$PREFIX" --target=i586-pc-msdosdjgpp --enable-languages=c,c++ \
    --batch --no-git-reset binutils-2.42 djgpp-cvs gcc-14.2.0

"$PREFIX/bin/i586-pc-msdosdjgpp-gcc" --version | head -n 1
echo "build-djgpp: DJGPP is in $PREFIX (make finds it there, or make DJGPP=$PREFIX)"
