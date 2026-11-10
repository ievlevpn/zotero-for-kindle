#!/bin/sh
# The plugin registry's release (registry/registry.json):
#   tools/registry.sh sign      check registry/registry.json, stamp "updated", write
#                               registry/out/registry.json + registry.json.sig, signed with
#                               local/release-key (the app's release key; never committed)
#   tools/registry.sh publish   gh release create on ievlevpn/zotero-for-kindle-plugins with those two
#                               assets. This is outward: run it only when you mean to publish.
set -eu
cd "$(dirname "$0")/.."
OUT=registry/out
case "${1:-}" in
  sign)
    [ -f local/release-key ] || { echo "error: local/release-key missing (the signing key)" >&2; exit 1; }
    node registry/check.mjs registry/registry.json
    ninja -C build-desktop zk-sign >/dev/null
    mkdir -p "$OUT"
    NOW=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    node -e 'const f=require("fs"),j=JSON.parse(f.readFileSync(process.argv[1],"utf8"));j.updated=process.argv[2];f.writeFileSync(process.argv[3],JSON.stringify(j,null,2)+"\n")' \
      registry/registry.json "$NOW" "$OUT/registry.json"
    build-desktop/zk-sign sign local/release-key "$OUT/registry.json" > "$OUT/registry.json.sig"
    build-desktop/zk-sign verify "$OUT/registry.json" "$OUT/registry.json.sig" >/dev/null
    echo "signed $OUT/registry.json ($NOW)"
    ;;
  publish)
    [ -f "$OUT/registry.json.sig" ] || { echo "error: run 'tools/registry.sh sign' first" >&2; exit 1; }
    TAG="registry-$(date -u +%Y%m%d-%H%M%S)"
    gh release create "$TAG" "$OUT/registry.json" "$OUT/registry.json.sig" \
      --repo ievlevpn/zotero-for-kindle-plugins --title "$TAG" --notes "The plugin registry."
    ;;
  *)
    echo "usage: tools/registry.sh sign | publish" >&2
    exit 2
    ;;
esac
