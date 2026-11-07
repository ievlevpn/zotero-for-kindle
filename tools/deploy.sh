#!/bin/sh
# Copies the Kindle build to the device over USBNetLite: the app, zk-sync, the CA bundle if
# missing, and the launcher scriptlet (shown in the Kindle library as "Zotero").
#   tools/deploy.sh [extra files to put in /mnt/us/zotero/bin...]
#   KINDLE_HOSTKEY=kindle-scribe tools/deploy.sh   another Kindle on the same USB address: its own
#                                                  pinned host key (ssh HostKeyAlias)
set -eu
cd "$(dirname "$0")/.."
KINDLE=${KINDLE:-root@192.168.15.244}
OPTS=""
[ -n "${KINDLE_HOSTKEY:-}" ] && OPTS="-o HostKeyAlias=$KINDLE_HOSTKEY"
ssh $OPTS "$KINDLE" 'mkdir -p /mnt/us/zotero/bin /mnt/us/zotero/lib /mnt/us/zotero/share /mnt/us/zotero/log /mnt/us/zotero/files'
D=build-kindle/dist  # stripped by tools/build-kindle.sh
[ -d "$D" ] || D=build-kindle
scp $OPTS -q "$D/zotero" "$D/zk-sync" "$@" "$KINDLE:/mnt/us/zotero/bin/"
ls "$D"/libzk-*.so >/dev/null 2>&1 && scp $OPTS -q "$D"/libzk-*.so "$KINDLE:/mnt/us/zotero/lib/"
ssh $OPTS "$KINDLE" 'mkdir -p /mnt/us/zotero/cache /mnt/us/zotero/fonts /mnt/us/zotero/share/crengine'
# crengine's hyphenation dictionaries and CSS (scp: the Kindle has no rsync).
scp $OPTS -q -r third_party/crengine/cr3gui/data/hyph third_party/crengine/cr3gui/data/epub.css \
    third_party/crengine/cr3gui/data/html5.css "$KINDLE:/mnt/us/zotero/share/crengine/"
ssh $OPTS "$KINDLE" '[ -f /mnt/us/zotero/share/crengine/hyph/English_US.pattern ] || { echo "error: crengine data not installed" >&2; exit 1; }'
scp $OPTS -q assets/launcher/icon.png "$KINDLE:/mnt/us/zotero/share/icon.png"
scp $OPTS -q assets/launcher/Zotero.sh "$KINDLE:/mnt/us/documents/Zotero.sh"
ssh $OPTS "$KINDLE" 'chmod +x /mnt/us/zotero/bin/* /mnt/us/documents/Zotero.sh; [ -f /mnt/us/zotero/share/cacert.pem ] || echo "warning: /mnt/us/zotero/share/cacert.pem missing (HTTPS will fail)"'
echo "deployed to $KINDLE:/mnt/us/zotero/bin"
