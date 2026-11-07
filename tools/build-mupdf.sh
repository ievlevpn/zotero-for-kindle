#!/bin/sh
# Builds MuPDF (static, position-independent, with its bundled third-party libraries) into
# build-deps/<platform>/prefix, for the PDF engine module (libzk-pdf.so).
#   tools/build-mupdf.sh desktop
#   tools/build-mupdf.sh kindle      # inside the zk-kindlehf container
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
PLATFORM=${1:?usage: build-mupdf.sh desktop|kindle}
VERSION=1.24.10
OUT=$ROOT/build-deps/$PLATFORM
PREFIX=$OUT/prefix
SRC=$OUT/src/mupdf-$VERSION-source
mkdir -p "$PREFIX/lib" "$PREFIX/include" "$OUT/src"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
[ -f "$PREFIX/lib/libmupdf.a" ] && { echo "mupdf already in $PREFIX"; exit 0; }
[ -d "$SRC" ] || (cd "$OUT/src" && curl -sL "https://mupdf.com/downloads/archive/mupdf-$VERSION-source.tar.gz" | tar xz)

ARGS="HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_OBJCOPY=no USE_TESSERACT=no USE_ZXINGCPP=no build=release"
# No JavaScript (mujs) — forms/JS aren't needed to read; smaller module.
XCFLAGS="-fPIC -DFZ_ENABLE_JS=0 -DFZ_ENABLE_XPS=0 -DFZ_ENABLE_SVG=0 -DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 -DFZ_ENABLE_HTML=0 -DFZ_ENABLE_EPUB=0 -DFZ_ENABLE_FB2=0 -DFZ_ENABLE_MOBI=0 -DFZ_ENABLE_OFFICE=0 -DTOFU_CJK_EXT -DNO_CJK"
if [ "$PLATFORM" = desktop ]; then
    # MuPDF's bundled zlib 1.2 clashes with the current macOS SDK (fdopen); use the system's.
    ARGS="$ARGS USE_SYSTEM_ZLIB=yes"
fi
if [ "$PLATFORM" = kindle ]; then
    T=arm-kindlehf-linux-gnueabihf
    ARGS="$ARGS OS=Linux CC=$T-gcc CXX=$T-g++ AR=$T-ar LD=$T-ld"
    XCFLAGS="$XCFLAGS -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard"
fi
make -C "$SRC" -j"$JOBS" $ARGS XCFLAGS="$XCFLAGS" libs >/dev/null
cp "$SRC"/build/release/libmupdf.a "$SRC"/build/release/libmupdf-third.a "$PREFIX/lib/"
cp -R "$SRC/include/mupdf" "$PREFIX/include/"
echo "mupdf ready in $PREFIX"
