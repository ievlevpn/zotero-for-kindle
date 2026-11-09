#!/bin/sh
# Checks the plugin SDK from a clean checkout: installs its pinned tools, type-checks zk.d.ts,
# then for the template and every folder in examples/ type-checks it against zk.d.ts (its own
# tsconfig.json, extending ../tsconfig.json), validates its plugin.json against the manifest
# schema, and builds it with its build.mjs.
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
  "$BIN/tsc" -p "$dir"
  node validate-manifest.mjs "$dir/plugin.json"
  node "$dir/build.mjs"
done
echo "sdk: all checks passed"
