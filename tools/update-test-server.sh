#!/bin/sh
# A local update feed for trying updates on a Kindle over USBNetwork, without GitHub.
#   tools/update-test-server.sh VERSION [--broken]
# Builds and signs a release that says VERSION (meson.build is put back afterwards) into
# dist/test-feed/, with latest.json in GitHub's shape, and serves it on the Mac's USBNetwork
# address. On the Kindle: echo http://192.168.15.201:8642/latest.json > /mnt/us/zotero/update-feed
# --broken: the release's bin/zotero exits at once (tries the launcher's rollback).
set -eu
cd "$(dirname "$0")/.."
VERSION=$1
BROKEN=${2:-}
HOST=${ZK_FEED_HOST:-192.168.15.201}
PORT=8642
OUT=dist/test-feed
cp meson.build /tmp/zk-meson.build.bak
trap 'cp /tmp/zk-meson.build.bak meson.build' EXIT
CURRENT=$(sed -n "s/^  version : '\(.*\)',$/\1/p" meson.build)
sed -i '' "s/^  version : '$CURRENT',$/  version : '$VERSION',/" meson.build
tools/build-kindle.sh
tools/package.sh
cp /tmp/zk-meson.build.bak meson.build
rm -rf "$OUT" && mkdir -p "$OUT"
cp dist/zotero-for-kindle.zip "$OUT/"
if [ "$BROKEN" = --broken ]; then
    T=$(mktemp -d)
    (cd "$T" && unzip -q "$OLDPWD/$OUT/zotero-for-kindle.zip" && printf '#!/bin/sh\nexit 1\n' > zotero/bin/zotero &&
        rm "$OLDPWD/$OUT/zotero-for-kindle.zip" && zip -qr -X "$OLDPWD/$OUT/zotero-for-kindle.zip" zotero documents)
    rm -rf "$T"
fi
tools/sign-release.sh "$OUT" "$VERSION"
python3 - "$OUT" "$VERSION" "http://$HOST:$PORT" <<'PY'
import json, os, sys
out, version, base = sys.argv[1:]
assets = [{"name": n, "size": os.path.getsize(os.path.join(out, n)), "browser_download_url": f"{base}/{n}"}
          for n in ("zotero-for-kindle.zip", "update.json", "update.json.sig")]
notes = f"## Test release {version}\n\n- Served from the Mac for an update test."
json.dump({"tag_name": f"v{version}", "body": notes, "assets": assets}, open(os.path.join(out, "latest.json"), "w"))
PY
echo "serving $OUT at http://$HOST:$PORT/latest.json (Ctrl-C to stop)"
exec python3 -m http.server "$PORT" --bind "$HOST" --directory "$OUT"
