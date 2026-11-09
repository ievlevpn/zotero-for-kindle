#!/bin/sh
# The release archive: the folders to copy onto the Kindle's USB drive, as they go.
#   tools/build-kindle.sh && tools/package.sh     -> dist/zotero-for-kindle.zip
#     zotero/      the app (bin, lib, share)       -> the Kindle's root
#     documents/   Zotero.sh and Zotero-no-plugins.sh, the library launchers -> the Kindle's root
set -eu
cd "$(dirname "$0")/.."
D=build-kindle/dist
for f in zotero zk-sync libzk-epub.so libzk-pdf.so; do
    [ -f "$D/$f" ] || { echo "error: $D/$f missing (run tools/build-kindle.sh)" >&2; exit 1; }
done
CA=build-deps/kindle/prefix/share/cacert.pem
[ -f "$CA" ] || { echo "error: $CA missing (run tools/build-net-deps.sh)" >&2; exit 1; }
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
Z="$STAGE/zotero"
mkdir -p "$Z/bin" "$Z/lib" "$Z/share/crengine" "$Z/share/fonts" "$STAGE/documents"
cp "$D/zotero" "$D/zk-sync" "$Z/bin/"
cp "$D"/libzk-*.so "$Z/lib/"
cp -R third_party/crengine/cr3gui/data/hyph third_party/crengine/cr3gui/data/epub.css \
    third_party/crengine/cr3gui/data/html5.css "$Z/share/crengine/"
cp assets/launcher/icon.png "$CA" "$Z/share/"
cp assets/fonts-fast/* "$Z/share/fonts/"  # Fast Font (MIT), with its LICENSE
cp assets/launcher/Zotero.sh assets/launcher/Zotero-no-plugins.sh "$STAGE/documents/"
cp LICENSE "$Z/"
chmod +x "$Z"/bin/* "$STAGE/documents/Zotero.sh" "$STAGE/documents/Zotero-no-plugins.sh"
mkdir -p dist
OUT="$PWD/dist/zotero-for-kindle.zip"  # one name for every release: a stable latest/ link
rm -f "$OUT" dist/update.json dist/update.json.sig  # a new zip: any old signature is for another one
(cd "$STAGE" && zip -qr -X "$OUT" zotero documents)
echo "$OUT ($(du -h "$OUT" | cut -f1))"
