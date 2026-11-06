#!/bin/sh
# Cross-builds the app for the Kindle (kindlehf) inside the zk-kindlehf container.
#   tools/build-kindle.sh [extra meson -D options...]
# crengine and its dependencies are built first (tools/build-crengine.sh kindle) if missing.
set -eu
cd "$(dirname "$0")/.."
EXTRA="$*"
docker run --rm -v "$PWD":/src -w /src zk-kindlehf sh -c "
  set -e
  TC=\$HOME/x-tools/arm-kindlehf-linux-gnueabihf
  case '$EXTRA' in *spikes=true*) [ -f build-deps/kindle/prefix/lib/libcrengine.a ] || tools/build-crengine.sh kindle ;; esac
  if [ ! -d build-kindle ]; then
    meson setup build-kindle --cross-file \$TC/meson-crosscompile.txt -Dplatform=kindle -Dtests=false $EXTRA
  elif [ -n '$EXTRA' ]; then
    meson configure build-kindle $EXTRA
  fi
  ninja -C build-kindle
"
