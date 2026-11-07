#!/bin/sh
# Writes and signs a release's manifest next to its zip (the format src/update/manifest.h reads):
#   tools/sign-release.sh DIR VERSION   -> DIR/update.json, DIR/update.json.sig
# Signs with local/release-key (never committed; keep a backup). Verifies the result with the
# public key built into the app.
set -eu
cd "$(dirname "$0")/.."
DIR=$1
VERSION=$2
ZIP="$DIR/zotero-for-kindle.zip"
[ -f "$ZIP" ] || { echo "error: $ZIP missing" >&2; exit 1; }
[ -f local/release-key ] || { echo "error: local/release-key missing (the signing key)" >&2; exit 1; }
ninja -C build-desktop zk-sign >/dev/null
SIZE=$(wc -c < "$ZIP" | tr -d ' ')
SHA=$(shasum -a 512 "$ZIP" | cut -d' ' -f1)
printf '{"version":"%s","zip":{"name":"zotero-for-kindle.zip","size":%s,"sha512":"%s"}}' "$VERSION" "$SIZE" "$SHA" \
    > "$DIR/update.json"
build-desktop/zk-sign sign local/release-key "$DIR/update.json" > "$DIR/update.json.sig"
build-desktop/zk-sign verify "$DIR/update.json" "$DIR/update.json.sig" >/dev/null
echo "signed $DIR/update.json ($VERSION, $SIZE bytes)"
