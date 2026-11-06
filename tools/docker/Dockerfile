# Kindle cross toolchain: koxtoolchain target kindlehf (firmware >= 5.16.3, hard float).
# Stage 1 of the zk-kindlehf image; kindle-sdk needs loop mounts, so it runs afterwards
# in a privileged container (tools/docker/setup-image.sh) and is committed on top.
FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential autoconf automake bison flex gawk libtool libtool-bin \
      libncurses-dev curl wget ca-certificates file git gperf help2man texinfo \
      unzip xz-utils patch rsync python3 python3-pip meson ninja-build cmake \
      pkg-config sudo nano bc \
    && rm -rf /var/lib/apt/lists/*

# crosstool-ng refuses to run as root.
RUN useradd -m -s /bin/bash builder && echo 'builder ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/builder
USER builder
WORKDIR /home/builder

ARG KOX_REF=master
RUN git clone --depth 1 --branch ${KOX_REF} https://github.com/koreader/koxtoolchain.git \
    && cd koxtoolchain \
    && ./gen-tc.sh kindlehf \
    && rm -rf build

RUN git clone --recursive --depth 1 https://github.com/KindleModding/kindle-sdk.git

ENV PATH=/home/builder/x-tools/arm-kindlehf-linux-gnueabihf/bin:${PATH}
WORKDIR /src
