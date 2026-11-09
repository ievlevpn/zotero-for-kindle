#!/bin/sh
# Generates the API reference (Markdown, for GitHub) from types/zk.d.ts with TypeDoc, as
# configured in typedoc.json. TypeDoc and its TypeScript are pinned in docs-tools/ (TypeDoc needs
# TypeScript's JavaScript API, which the SDK's TypeScript 7 doesn't have).
#
#   sdk/reference.sh          # regenerate docs/plugins/reference/ (commit the result)
#   sdk/reference.sh <dir>    # generate into <dir> instead (check.sh compares the two)
set -eu
out=${1:-}
case $out in "" | /*) ;; *) out="$PWD/$out" ;; esac
cd "$(dirname "$0")"
out=${out:-../docs/plugins/reference}
(cd docs-tools && npm ci --ignore-scripts --no-audit --no-fund --loglevel=error)
docs-tools/node_modules/.bin/typedoc --options typedoc.json --out "$out" --logLevel Warn
