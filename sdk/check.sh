#!/bin/sh
# Checks the plugin SDK from a clean checkout: installs its pinned tools, type-checks zk.d.ts,
# then for the template and every folder in examples/ installs its own npm packages (if it has a
# package-lock.json), type-checks it against zk.d.ts (its own tsconfig.json, extending the SDK's
# tsconfig.json), validates its plugin.json against the manifest schema, and builds it with its
# build.mjs. It then checks that the example snapshots committed under tests/data/plugins/examples/
# equal a fresh build (they can't drift silently). Finally it checks that build.mjs bundles an npm package (test-fixtures/).
#
#   sdk/check.sh
set -eu
cd "$(dirname "$0")"
npm ci --ignore-scripts --no-audit --no-fund --loglevel=error
BIN=node_modules/.bin

echo "== types"
"$BIN/tsc" -p tsconfig.json

for dir in template examples/*/; do
  [ -d "$dir" ] || continue
  dir=${dir%/}
  echo "== $dir"
  # a plugin that bundles npm packages pins them in its own package-lock.json
  if [ -f "$dir/package-lock.json" ]; then
    (cd "$dir" && npm ci --omit=dev --ignore-scripts --no-audit --no-fund --loglevel=error)
  fi
  "$BIN/tsc" -p "$dir"
  node validate-manifest.mjs "$dir/plugin.json"
  node "$dir/build.mjs"
done
echo "== committed example fixtures match a fresh build"
# tests/data/plugins/examples/<id>/ is a snapshot of examples/<id>/dist/<id>/ that the app's tests
# load; a snapshot that differs from a fresh build fails here. Refresh it by copying the build.
drift=0
for fixture in ../tests/data/plugins/examples/*/; do
  [ -d "$fixture" ] || continue
  id=$(basename "$fixture")
  if [ ! -d "examples/$id/dist/$id" ]; then
    echo "fixture $id has no example in sdk/examples/" >&2
    drift=1
  elif ! diff -r "$fixture" "examples/$id/dist/$id" >/dev/null; then
    echo "fixture tests/data/plugins/examples/$id differs from a fresh build of sdk/examples/$id:" >&2
    diff -r "$fixture" "examples/$id/dist/$id" >&2 || true
    echo "  refresh: cp examples/$id/dist/$id/* ../tests/data/plugins/examples/$id/" >&2
    drift=1
  else
    echo "$id: same"
  fi
done
[ "$drift" = 0 ] || exit 1
echo "== build.mjs bundles an npm package (one with only a CommonJS \"main\")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -R test-fixtures/bundles-a-package "$tmp/plugin"
mkdir -p "$tmp/plugin/node_modules"
cp -R test-fixtures/main-only-package "$tmp/plugin/node_modules/"
node template/build.mjs "$tmp/plugin"
out=$(node --input-type=module -e "globalThis.zk = {};
  await import('$tmp/plugin/dist/bundles-a-package/main.js');")
[ "$out" = "bundled hello, Kindle" ] || { echo "bundled plugin printed: $out" >&2; exit 1; }
echo "$out"

echo "sdk: all checks passed"
