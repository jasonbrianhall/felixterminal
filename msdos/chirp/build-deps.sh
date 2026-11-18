#!/bin/sh
# Build the libraries Felix Chirp for DOS (CHIRP.EXE) links with, for DJGPP,
# into msdos/chirp/deps (or DEPS=dir): FreeType, libwebp (decoder), libxmp
# (tracker music), and the single-file decoders dr_mp3, dr_flac, dr_wav
# (dr_libs) and stb_vorbis. Run once (`make` does); SDL3 is in sdl3/ and the
# Makefile compiles it.
#
# Needs:
#   a DJGPP cross compiler (i586-pc-msdosdjgpp-gcc/g++ on PATH, or DJGPP=dir)
#   git, make
#
# Unlike FLT there is no OpenSSL, libssh2 or vmdos socket layer: Chirp on DOS
# browses local drives only.
set -e
here=$(cd "$(dirname "$0")" && pwd)
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
P=i586-pc-msdosdjgpp-
command -v ${P}gcc > /dev/null || { echo "build-deps: no ${P}gcc (DJGPP=dir?)" >&2; exit 1; }
DEPS=${DEPS:-$here/deps}
mkdir -p "$DEPS" && DEPS=$(cd "$DEPS" && pwd)
S=$DEPS/src
mkdir -p "$S" "$DEPS/lib" "$DEPS/include"
CF="-O2 -march=i586 -w"

clone() {   # url tag dir
    [ -d "$S/$3" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$S/$3"
}
clone_rev() {   # url rev dir
    [ -d "$S/$3" ] || { git -c advice.detachedHead=false clone -q "$1" "$S/$3" &&
        git -C "$S/$3" -c advice.detachedHead=false checkout -q "$2"; }
}

# FreeType: TrueType, the smooth rasterizer and the autohinter (the fonts are
# embedded TTFs; no zlib needed)
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
     ${P}gcc $CF -DFT2_BUILD_LIBRARY "-DFT_CONFIG_MODULES_H=<ftmodule.h>" \
         -Idjcfg -Iinclude -c src/$s.c -o o/$(basename $s).o || exit 1; done &&
 ${P}ar rcs "$DEPS/lib/libfreetype.a" o/*.o &&
 mkdir -p "$DEPS/include/freetype2" && cp -r include/* djcfg/ftmodule.h "$DEPS/include/freetype2/")

# libwebp: the decoder only (WebPDecodeRGBA), plain C (no SSE/threads)
clone https://github.com/webmproject/libwebp v1.5.0 libwebp
(cd "$S/libwebp" && mkdir -p dj && rm -f dj/*.o &&
 for f in src/dec/*.c src/utils/*.c src/dsp/*.c; do
     case $f in *_sse2.c|*_sse41.c|*_neon.c|*_msa.c|*_mips*.c|*enc*.c|*/cost.c|*/ssim.c|*bit_writer*|*huffman_encode*|*quant_levels_utils.c) continue;; esac
     ${P}gcc $CF -I. -Isrc -c $f -o dj/$(echo $f | tr / _).o || exit 1; done &&
 ${P}ar rcs "$DEPS/lib/libwebp.a" dj/*.o &&
 mkdir -p "$DEPS/include/webp" && cp src/webp/decode.h src/webp/types.h "$DEPS/include/webp/")

# libxmp: MOD, XM, IT, S3M, 669, MED, MTM and the rest
clone https://github.com/libxmp/libxmp libxmp-4.6.3 libxmp
(cd "$S/libxmp" && mkdir -p dj && rm -f dj/*.o &&
 lhasa=$(for f in ext_header lha_file_header lha_input_stream lha_decoder lha_reader lha_basic_reader \
                 lh1_decoder lh5_decoder lh6_decoder lh7_decoder lhx_decoder lk7_decoder lz5_decoder \
                 lzs_decoder null_decoder pm1_decoder pm2_decoder macbinary; do echo src/depackers/lhasa/$f.c; done) &&
 for f in src/*.c src/loaders/*.c src/depackers/*.c $lhasa; do
     ${P}gcc $CF -DLIBXMP_STATIC -DBUILDING_STATIC -DLIBXMP_NO_PROWIZARD -Iinclude -Isrc -c $f -o dj/$(echo $f | tr / _).o || exit 1; done &&
 ${P}ar rcs "$DEPS/lib/libxmp.a" dj/*.o && cp include/xmp.h "$DEPS/include/")

# dr_mp3, dr_flac, dr_wav and stb_vorbis: single files, compiled in mixer_dos.cpp
clone_rev https://github.com/mackron/dr_libs 0901cdc54e719e2175827cb60b78fe725e581fe2 dr_libs
clone_rev https://github.com/nothings/stb 9600522de53973336fa033a289858a4354cbb04c stb
cp "$S/dr_libs/dr_mp3.h" "$S/dr_libs/dr_flac.h" "$S/dr_libs/dr_wav.h" "$S/stb/stb_vorbis.c" "$DEPS/include/"

touch "$DEPS/.built"
echo "build-deps: done ($DEPS)"
