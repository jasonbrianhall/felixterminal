#!/bin/sh
# Build the libraries Felix Terminal for DOS links with, for DJGPP, into
# msdos/deps (or DEPS=dir): zlib, libpng, FreeType, OpenSSL, libssh2, SDL3
# (the DOS port in SDLPoPDOS) and vmdos's sockets. Run once; then `make`.
#
# Needs:
#   a DJGPP cross compiler (i586-pc-msdosdjgpp-gcc/g++ on PATH, or DJGPP=dir)
#   VMDOS=dir   a vmdos checkout (github.com/jasonbrianhall/vmdos): its
#               OpenSSL build (tools/build-openssl.sh) and its sockets
#               (third_party/openssh/dos-sock.c, third_party/openssl/ossl-dos.c),
#               which put TCP on vmdos's own network stack
#   SDLDOS=dir  SDLPoPDOS's msdos/ (github.com/jasonbrianhall/SDLPoPDOS): SDL3
#               with its DOS port (VESA, keyboard, mouse)
#   git, make, perl, python3, a host gcc with -m32 (for OpenSSL's build)
#
# The sources are cloned at pinned versions into msdos/deps/src. `make deps`
# (or just `make`) runs this, in Docker too (DOCKER=1; see the Makefile).
set -e
here=$(cd "$(dirname "$0")" && pwd)
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
P=i586-pc-msdosdjgpp-   # (vmdos's build-openssl.sh wants this name too)
command -v ${P}gcc > /dev/null || { echo "build-deps: no ${P}gcc (DJGPP=dir?)" >&2; exit 1; }
: "${VMDOS:?VMDOS=dir: a vmdos checkout}"
: "${SDLDOS:?SDLDOS=dir: SDLPoPDOS/msdos}"
[ -d "$VMDOS" ] || { echo "build-deps: no vmdos checkout at $VMDOS (VMDOS=dir)" >&2; exit 1; }
[ -d "$SDLDOS" ] || { echo "build-deps: no SDLPoPDOS msdos/ at $SDLDOS (SDLDOS=dir)" >&2; exit 1; }
VMDOS=$(cd "$VMDOS" && pwd); SDLDOS=$(cd "$SDLDOS" && pwd)
DEPS=${DEPS:-$here/deps}
mkdir -p "$DEPS" && DEPS=$(cd "$DEPS" && pwd)
S=$DEPS/src
mkdir -p "$S" "$DEPS/lib" "$DEPS/include"
CF="-O2 -march=i586"

clone() {   # url tag dir
    [ -d "$S/$3" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$S/$3"
}

# zlib
clone https://github.com/madler/zlib v1.3.1 zlib
(cd "$S/zlib" && CC=${P}gcc AR=${P}ar RANLIB=${P}ranlib CFLAGS="$CF" \
    ./configure --static --prefix="$DEPS" > /dev/null && make -s libz.a && make -s install > /dev/null)

# libpng (kitty's PNG images)
clone https://github.com/pnggroup/libpng v1.6.47 libpng
(cd "$S/libpng" && cp scripts/pnglibconf.h.prebuilt pnglibconf.h && mkdir -p o &&
 for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset \
          pngtrans pngwio pngwrite pngwtran pngwutil; do
     ${P}gcc $CF -DPNG_INTEL_SSE_OPT=0 -I. -I"$DEPS/include" -c $f.c -o o/$f.o || exit 1; done &&
 ${P}ar rcs "$DEPS/lib/libpng.a" o/*.o && cp png.h pngconf.h pnglibconf.h "$DEPS/include/")

# FreeType: TrueType, the smooth rasterizer and the autohinter are all a
# terminal font needs
clone https://github.com/freetype/freetype VER-2-13-3 freetype
(cd "$S/freetype" && mkdir -p djcfg o && cat > djcfg/ftmodule.h <<'EOF'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
EOF
 for s in base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbitmap base/ftglyph base/ftmm \
          truetype/truetype sfnt/sfnt smooth/smooth psnames/psnames autofit/autofit gzip/ftgzip; do
     ${P}gcc $CF -DFT2_BUILD_LIBRARY "-DFT_CONFIG_MODULES_H=<ftmodule.h>" -DFT_CONFIG_OPTION_SYSTEM_ZLIB \
         -Idjcfg -Iinclude -I"$DEPS/include" -c src/$s.c -o o/$(basename $s).o || exit 1; done &&
 ${P}ar rcs "$DEPS/lib/libfreetype.a" o/*.o &&
 mkdir -p "$DEPS/include/freetype2" && cp -r include/* djcfg/ftmodule.h "$DEPS/include/freetype2/")

# Watt-32's headers (sockets, errno) for OpenSSL, libssh2 and vmdos's
# sockets; none of its code is linked. The revision vmdos's builds use.
[ -d "$S/Watt-32" ] || { git -c advice.detachedHead=false clone -q https://github.com/gvanem/Watt-32 "$S/Watt-32" &&
    git -C "$S/Watt-32" -c advice.detachedHead=false checkout -q 3cf6f4e504b46bea2302b8d88b5ff2431396ddc9; }
W=$S/Watt-32
dj=$(dirname "$(command -v ${P}gcc)")/..
inc=$dj/i586-pc-msdosdjgpp/sys-include
python3 "$VMDOS/third_party/openssh/djstrerr.py" "$inc/errno.h" > "$S/djstrerr.h"
gcc -m32 -w -DWATT32_DJGPP_MINGW --include "$inc/errno.h" -D_ERRNO_H -D_BITS_ERRNO_H \
    --include "$inc/sys/version.h" -include "$S/djstrerr.h" -idirafter "$W/inc" \
    -o "$S/dj_err" "$W/util/errnos.c"
(cd "$W/src" && sh configur.sh djgpp > /dev/null && "$S/dj_err" -e > ../inc/sys/djgpp.err)

# OpenSSL: vmdos's DJGPP build of it (libraries only)
[ -f "$DEPS/lib/libcrypto.a" ] || OPENSSL_PREFIX="$DEPS" NO_EXE=1 WATTSRC="$W" sh "$VMDOS/tools/build-openssl.sh"

# libssh2 on OpenSSL; sockets made non-blocking with vmdos's ioctlsocket()
clone https://github.com/libssh2/libssh2 libssh2-1.11.1 libssh2
(cd "$S/libssh2" && python3 "$here/libssh2-dos.py" src/session.c &&
 mkdir -p dj/o && cp "$here/libssh2_config.h" dj/ &&
 for f in agent bcrypt_pbkdf channel comp crypt global hostkey keepalive kex knownhost mac misc \
          crypto packet pem publickey scp session sftp transport userauth userauth_kbd_packet \
          version chacha cipher-chachapoly poly1305 blowfish; do   # crypto.c: openssl.c inside
     ${P}gcc $CF -DHAVE_CONFIG_H -DWATT32 -DWATT32_NO_OLDIES -Idj -Iinclude -Isrc -I"$W/inc" \
         -I"$DEPS/include" -include "$VMDOS/third_party/openssl/dos-time.h" -c src/$f.c -o dj/o/$f.o || exit 1; done &&
 ${P}ar rcs "$DEPS/lib/libssh2.a" dj/o/*.o && cp include/*.h "$DEPS/include/")

# vmdos's sockets: TCP and DNS through vmdos (INT 2Fh AX=564Eh), and what
# OpenSSL and libssh2 want around them (send/recv/select, getentropy, the clock)
V=$VMDOS/third_party
GF="$CF -I$V/wget/dos -I$V/openssh -I$W/inc -DWATT32 -DWATT32_NO_OLDIES"
${P}gcc $GF -c "$V/openssl/ossl-dos.c" -o "$S/ossl-dos.o"
${P}gcc $GF -c "$V/openssh/dos-sock.c" -o "$S/dos-sock.o"
${P}ar rcs "$DEPS/lib/libvmdosnet.a" "$S/ossl-dos.o" "$S/dos-sock.o"

# SDL3 with its DOS port: SDLPoPDOS's build of it
make -s -C "$SDLDOS" DOCKER=0 build/libSDL3.a
cp "$SDLDOS/build/libSDL3.a" "$DEPS/lib/"
cp -r "$SDLDOS/SDL3/include/SDL3" "$DEPS/include/"

touch "$DEPS/.built"
echo "build-deps: done ($DEPS)"
