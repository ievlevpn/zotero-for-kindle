#!/bin/sh
# Static builds of crengine's dependencies for kindlehf (called by build-crengine.sh kindle).
# The firmware's freetype/harfbuzz are too old for crengine (needs harfbuzz >= 8), so crengine
# gets its own static copies. Keeping them apart from the firmware copies pango uses needs a
# real boundary (Plan 4: crengine as its own .so with -Bsymbolic, or ld -r + localized symbols);
# -Wl,--exclude-libs,ALL alone does not decide which copy crengine binds to.
#   build-kindle-deps.sh <src dir> <prefix> <jobs>
set -eu
SRC=$1 PREFIX=$2 JOBS=$3
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC="-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/kindlehf.cmake"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=""
CFLAGS="-O2 -fPIC -march=armv7-a -mfpu=neon -mfloat-abi=hard"; export CFLAGS CXXFLAGS="$CFLAGS"

fetch() {  # url dir
    [ -d "$SRC/$2" ] && return 0
    case "$1" in *.xz) (cd "$SRC" && curl -sfL "$1" | tar xJ) ;; *) (cd "$SRC" && curl -sfL "$1" | tar xz) ;; esac
}
have() { [ -f "$PREFIX/lib/$1" ]; }
cm() {  # dir [cmake args]
    d=$1; shift
    cmake -S "$SRC/$d" -B "$SRC/$d/build-kindle" -G Ninja $TC -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
    ninja -C "$SRC/$d/build-kindle" install >/dev/null
}

cat > "$SRC/kindle-cross.ini" <<EOF
[binaries]
c = 'arm-kindlehf-linux-gnueabihf-gcc'
cpp = 'arm-kindlehf-linux-gnueabihf-g++'
ar = 'arm-kindlehf-linux-gnueabihf-ar'
strip = 'arm-kindlehf-linux-gnueabihf-strip'
pkgconfig = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
EOF

if ! have libz.a; then
    fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz zlib-1.3.1
    (cd "$SRC/zlib-1.3.1" && CC=arm-kindlehf-linux-gnueabihf-gcc ./configure --static --prefix="$PREFIX" >/dev/null \
        && make -j"$JOBS" install >/dev/null)
fi
have libpng16.a || { fetch https://download.sourceforge.net/libpng/libpng-1.6.43.tar.gz libpng-1.6.43
    cm libpng-1.6.43 -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=on; }
have libjpeg.a || { fetch https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.3/libjpeg-turbo-3.0.3.tar.gz libjpeg-turbo-3.0.3
    cm libjpeg-turbo-3.0.3 -DENABLE_SHARED=OFF -DWITH_TURBOJPEG=OFF -DCMAKE_INSTALL_LIBDIR=lib; }
have libzstd.a || { fetch https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz zstd-1.5.6
    cm zstd-1.5.6/build/cmake -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DCMAKE_INSTALL_LIBDIR=lib; }
have libxxhash.a || { fetch https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.2.tar.gz xxHash-0.8.2
    cm xxHash-0.8.2/cmake_unofficial -DXXHASH_BUILD_XXHSUM=OFF -DCMAKE_INSTALL_LIBDIR=lib; }
have libutf8proc.a || { fetch https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.9.0.tar.gz utf8proc-2.9.0
    cm utf8proc-2.9.0 -DUTF8PROC_ENABLE_TESTING=OFF -DCMAKE_INSTALL_LIBDIR=lib; }
have libfribidi.a || { fetch https://github.com/fribidi/fribidi/releases/download/v1.0.13/fribidi-1.0.13.tar.xz fribidi-1.0.13
    meson setup "$SRC/fribidi-1.0.13/build-kindle" "$SRC/fribidi-1.0.13" --cross-file "$SRC/kindle-cross.ini" \
        --prefix="$PREFIX" --libdir=lib --default-library=static -Ddocs=false -Dtests=false -Dbin=false >/dev/null
    ninja -C "$SRC/fribidi-1.0.13/build-kindle" install >/dev/null; }
have libfreetype.a || { fetch https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.gz freetype-2.13.2
    cm freetype-2.13.2 -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_REQUIRE_ZLIB=ON \
        -DFT_REQUIRE_PNG=ON -DCMAKE_INSTALL_LIBDIR=lib; }
have libharfbuzz.a || { fetch https://github.com/harfbuzz/harfbuzz/releases/download/8.5.0/harfbuzz-8.5.0.tar.xz harfbuzz-8.5.0
    meson setup "$SRC/harfbuzz-8.5.0/build-kindle" "$SRC/harfbuzz-8.5.0" --cross-file "$SRC/kindle-cross.ini" \
        --prefix="$PREFIX" --libdir=lib --default-library=static -Dtests=disabled -Ddocs=disabled \
        -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dicu=disabled -Dfreetype=enabled -Dutilities=disabled >/dev/null
    ninja -C "$SRC/harfbuzz-8.5.0/build-kindle" install >/dev/null; }
echo "kindle deps ready in $PREFIX"
