#!/bin/sh
# Builds crengine (third_party/crengine) and the dependencies it needs that the
# platform doesn't provide, as static libraries, into build-deps/<platform>/prefix.
#
#   tools/build-crengine.sh desktop   # macOS: other deps from Homebrew
#   tools/build-crengine.sh kindle    # inside the zk-kindlehf container (see build-kindle.sh)
#
# libunibreak is pinned to 7.0 like KOReader, and its internal headers are
# installed too: crengine includes linebreakdef.h.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
PLATFORM=${1:?usage: build-crengine.sh desktop|kindle}
OUT=$ROOT/build-deps/$PLATFORM
PREFIX=$OUT/prefix
SRC=$OUT/src
mkdir -p "$PREFIX/include" "$PREFIX/lib" "$SRC"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

case "$PLATFORM" in
    desktop) HOST_ARGS=""; CMAKE_TC="" ;;
    kindle)
        HOST_ARGS="--host=arm-kindlehf-linux-gnueabihf"
        CMAKE_TC="-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/kindlehf.cmake"
        export CC=arm-kindlehf-linux-gnueabihf-gcc CXX=arm-kindlehf-linux-gnueabihf-g++
        export AR=arm-kindlehf-linux-gnueabihf-ar RANLIB=arm-kindlehf-linux-gnueabihf-ranlib
        ;;
    *) echo "unknown platform $PLATFORM" >&2; exit 2 ;;
esac
if [ "$PLATFORM" = kindle ]; then
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""
else
    export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

fetch() {  # url dir
    [ -d "$SRC/$2" ] || (cd "$SRC" && curl -sL "$1" | tar xz)
}

autotools() {  # dir [configure args...]
    dir=$1; shift
    (cd "$SRC/$dir" && ./configure $HOST_ARGS --prefix="$PREFIX" --disable-shared --enable-static \
        --enable-silent-rules "$@" >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)
}

# --- libunibreak 7.0 (all platforms) ---
if [ ! -f "$PREFIX/lib/libunibreak.a" ]; then
    fetch https://github.com/adah1972/libunibreak/releases/download/libunibreak_7_0/libunibreak-7.0.tar.gz libunibreak-7.0
    autotools libunibreak-7.0
    cp "$SRC"/libunibreak-7.0/src/*def.h "$PREFIX/include/"
fi

if [ "$PLATFORM" = kindle ]; then
    sh "$ROOT/tools/build-kindle-deps.sh" "$SRC" "$PREFIX" "$JOBS"
fi

# --- crengine ---
BUILD=$OUT/crengine
cmake -S third_party/crengine -B "$BUILD" -G Ninja $CMAKE_TC \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DANTIWORD=OFF -DCHMLIB=OFF -DLUNASVG=OFF -DMD4C=OFF -DSRELL=OFF -DNANOSVG=OFF \
    -DLIBWEBP=OFF -DGIF=OFF -DFONTCONFIG=OFF >/dev/null
ninja -C "$BUILD" crengine >/dev/null
cp "$BUILD/libcrengine.a" "$PREFIX/lib/"
mkdir -p "$PREFIX/include/crengine"
cp -R third_party/crengine/crengine/include/. "$PREFIX/include/crengine/"
cp "$BUILD/crsetup.h" "$PREFIX/include/crengine/"
echo "crengine ready in $PREFIX"
