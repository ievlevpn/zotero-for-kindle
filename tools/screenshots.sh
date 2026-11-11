#!/bin/sh
# The README's screenshots, from a demo library of public-domain books (tools/demo_library.cpp).
#   tools/screenshots.sh        -> docs/screenshots/*.png
set -eu
cd "$(dirname "$0")/.."
ninja -C build-desktop zotero zk-demo-library >/dev/null
DB=local/demo/demo.sqlite
OUT=docs/screenshots
mkdir -p "$OUT"
build-desktop/zk-demo-library "$DB"
shot() { build-desktop/zotero --screenshot "$1" "$OUT/$2.png" --db "$DB" >/dev/null 2>&1; }
shot home home
shot notes notes
shot reader:PRIDEEPB:14 reader
shot reader:PRIDEEPB:14:menus reader-menus
build-desktop/zotero --screenshot reader:PRIDEEPB:5 "$OUT/scribe-landscape.png" --db "$DB" \
    --device scribe --landscape >/dev/null 2>&1
ls "$OUT"
