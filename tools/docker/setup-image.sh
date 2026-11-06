#!/bin/sh
# Builds the zk-kindlehf image: toolchain (Dockerfile), then kindle-sdk sysroot.
# kindle-sdk loop-mounts Amazon firmware, so that step runs in a privileged container.
set -eu
cd "$(dirname "$0")"

docker image inspect zk-kindlehf-tc >/dev/null 2>&1 || docker build -t zk-kindlehf-tc .

docker rm -f zk-sdk-setup >/dev/null 2>&1 || true
docker run --privileged --name zk-sdk-setup zk-kindlehf-tc \
    sh -c 'sudo apt-get update -qq && sudo apt-get install -y -qq libarchive-dev nettle-dev zlib1g-dev libbz2-dev liblzma-dev libssl-dev >/dev/null && cd ~/kindle-sdk && chmod +x gen-sdk.sh && ./gen-sdk.sh kindlehf && rm -rf cache'
docker commit zk-sdk-setup zk-kindlehf
docker rm zk-sdk-setup >/dev/null

docker run --rm zk-kindlehf sh -c 'ls ~/x-tools/arm-kindlehf-linux-gnueabihf/meson-crosscompile.txt && arm-kindlehf-linux-gnueabihf-gcc --version | head -1'
