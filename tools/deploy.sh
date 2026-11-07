#!/bin/sh
# Copies the Kindle build to the device over USBNetLite: the app, zk-sync, the CA bundle if
# missing, and the launcher scriptlet (shown in the Kindle library as "Zotero").
#   tools/deploy.sh [extra files to put in /mnt/us/zotero/bin...]
set -eu
cd "$(dirname "$0")/.."
KINDLE=${KINDLE:-root@192.168.15.244}
ssh "$KINDLE" 'mkdir -p /mnt/us/zotero/bin /mnt/us/zotero/lib /mnt/us/zotero/share /mnt/us/zotero/log /mnt/us/zotero/files'
D=build-kindle/dist  # stripped by tools/build-kindle.sh
[ -d "$D" ] || D=build-kindle
scp -q "$D/zotero" "$D/zk-sync" "$@" "$KINDLE:/mnt/us/zotero/bin/"
ls "$D"/libzk-*.so >/dev/null 2>&1 && scp -q "$D"/libzk-*.so "$KINDLE:/mnt/us/zotero/lib/"
ssh "$KINDLE" 'mkdir -p /mnt/us/zotero/cache /mnt/us/zotero/fonts; [ -d /mnt/us/zotero/share/crengine/hyph ] || echo "warning: crengine data missing in /mnt/us/zotero/share/crengine"'
rsync -a third_party/crengine/cr3gui/data/hyph third_party/crengine/cr3gui/data/epub.css third_party/crengine/cr3gui/data/html5.css "$KINDLE:/mnt/us/zotero/share/crengine/" 2>/dev/null || true
scp -q assets/launcher/icon.png "$KINDLE:/mnt/us/zotero/share/icon.png"
scp -q assets/launcher/Zotero.sh "$KINDLE:/mnt/us/documents/Zotero.sh"
ssh "$KINDLE" 'chmod +x /mnt/us/zotero/bin/* /mnt/us/documents/Zotero.sh; [ -f /mnt/us/zotero/share/cacert.pem ] || echo "warning: /mnt/us/zotero/share/cacert.pem missing (HTTPS will fail)"'
echo "deployed to $KINDLE:/mnt/us/zotero/bin"
