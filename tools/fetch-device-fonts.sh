#!/bin/sh
# Copies the Kindle UI and reading fonts from the device for desktop builds.
# Amazon fonts are not redistributable: assets/fonts-device/ is git-ignored.
set -eu
cd "$(dirname "$0")/.."
KINDLE=${KINDLE:-root@192.168.15.244}
mkdir -p assets/fonts-device
ssh "$KINDLE" 'cd /usr/java/lib/fonts && tar cf - Amazon-Ember-*.ttf Bookerly-*.ttf' | tar xf - -C assets/fonts-device
ls assets/fonts-device
