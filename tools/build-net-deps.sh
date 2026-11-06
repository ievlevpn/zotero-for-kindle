#!/bin/sh
# Static libcurl + mbedTLS for the Kindle, into build-deps/kindle/prefix (run in the zk-kindlehf container).
# The firmware's curl uses OpenSSL 1.0.2q; the app brings its own TLS stack and CA bundle.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
PREFIX=$ROOT/build-deps/kindle/prefix
SRC=$ROOT/build-deps/kindle/src
mkdir -p "$PREFIX/share" "$SRC"
TC="-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/kindlehf.cmake"
CFLAGS="-O2 -fPIC -march=armv7-a -mfpu=neon -mfloat-abi=hard"; export CFLAGS

cm() {
    d=$1; shift
    cmake -S "$SRC/$d" -B "$SRC/$d/build-kindle" -G Ninja $TC -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_LIBDIR=lib "$@" >/dev/null
    ninja -C "$SRC/$d/build-kindle" install >/dev/null
}

if [ ! -f "$PREFIX/lib/libmbedtls.a" ]; then
    [ -d "$SRC/mbedtls-3.6.2" ] || (cd "$SRC" && curl -sfL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 | tar xj)
    cm mbedtls-3.6.2 -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
fi
if [ ! -f "$PREFIX/lib/libcurl.a" ]; then
    [ -d "$SRC/curl-8.11.1" ] || (cd "$SRC" && curl -sfL https://curl.se/download/curl-8.11.1.tar.xz | tar xJ)
    cm curl-8.11.1 -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
        -DCURL_DISABLE_LDAP=ON -DUSE_LIBIDN2=OFF -DENABLE_WEBSOCKETS=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none \
        -DZLIB_USE_STATIC_LIBS=ON -DZLIB_ROOT="$PREFIX"
fi
curl -sfL https://curl.se/ca/cacert.pem -o "$PREFIX/share/cacert.pem"
echo "net deps ready in $PREFIX"
