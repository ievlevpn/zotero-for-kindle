#!/bin/sh
# Cuts a release: version, Kindle build, signed package, the tag, the GitHub release.
#   tools/release.sh [--dry-run] VERSION NOTES.md
# 1. master must be clean and VERSION newer than meson.build's; the version is set and committed.
# 2. tools/build-kindle.sh, tools/package.sh, tools/sign-release.sh dist VERSION.
# 3. "release VERSION" is tagged vVERSION; master and the tag are pushed to origin.
# 4. gh release create vVERSION with zotero-for-kindle.zip, update.json, update.json.sig.
# --dry-run stops after step 2 and puts meson.build back.
set -eu
cd "$(dirname "$0")/.."
DRY=0
[ "${1:-}" = --dry-run ] && { DRY=1; shift; }
[ $# -eq 2 ] || { echo "usage: tools/release.sh [--dry-run] VERSION NOTES.md" >&2; exit 2; }
VERSION=$1
NOTES=$2
[ -f "$NOTES" ] || { echo "error: $NOTES missing" >&2; exit 1; }
[ "$DRY" = 1 ] || [ "$(git branch --show-current)" = master ] || { echo "error: not on master" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "error: the working tree isn't clean" >&2; exit 1; }
CURRENT=$(sed -n "s/^  version : '\(.*\)',$/\1/p" meson.build)
python3 - "$CURRENT" "$VERSION" <<'PY' || { echo "error: $VERSION isn't newer than $CURRENT" >&2; exit 1; }
import re, sys
def v(s):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", s)
    if not m: sys.exit(1)
    return tuple(map(int, m.groups()))
sys.exit(0 if v(sys.argv[2]) > v(sys.argv[1]) else 1)
PY

sed -i '' "s/^  version : '$CURRENT',$/  version : '$VERSION',/" meson.build
restore() { if [ "$DRY" = 1 ]; then git checkout -q meson.build; fi; }  # (an exit trap's status is the script's)
trap restore EXIT
tools/build-kindle.sh
tools/package.sh
tools/sign-release.sh dist "$VERSION"
if [ "$DRY" = 1 ]; then
    echo "dry run: dist/ is ready; nothing committed, pushed or published"
    exit 0
fi

git commit -qm "release $VERSION" meson.build
git tag "v$VERSION"
git push -q origin master "v$VERSION"
gh release create "v$VERSION" --repo ievlevpn/zotero-for-kindle --verify-tag --title "Zotero for Kindle $VERSION" \
    --notes-file "$NOTES" dist/zotero-for-kindle.zip dist/update.json dist/update.json.sig
echo "released $VERSION"
