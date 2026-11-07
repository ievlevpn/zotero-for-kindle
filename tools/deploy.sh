#!/bin/sh
# Copies the Kindle build to the device over USBNetLite: the app, zk-sync, the CA bundle if
# missing, and the launcher scriptlet (shown in the Kindle library as "Zotero").
#   tools/deploy.sh [extra files to put in /mnt/us/zotero/bin...]
set -eu
cd "$(dirname "$0")/.."
KINDLE=${KINDLE:-root@192.168.15.244}
ssh "$KINDLE" 'mkdir -p /mnt/us/zotero/bin /mnt/us/zotero/lib /mnt/us/zotero/share /mnt/us/zotero/log /mnt/us/zotero/files'
scp -q build-kindle/zotero build-kindle/zk-sync "$@" "$KINDLE:/mnt/us/zotero/bin/"
ls build-kindle/libzk-*.so >/dev/null 2>&1 && scp -q build-kindle/libzk-*.so "$KINDLE:/mnt/us/zotero/lib/"
scp -q assets/launcher/icon.png "$KINDLE:/mnt/us/zotero/share/icon.png"
scp -q assets/launcher/Zotero.sh "$KINDLE:/mnt/us/documents/Zotero.sh"
ssh "$KINDLE" 'chmod +x /mnt/us/zotero/bin/* /mnt/us/documents/Zotero.sh; [ -f /mnt/us/zotero/share/cacert.pem ] || echo "warning: /mnt/us/zotero/share/cacert.pem missing (HTTPS will fail)"'
echo "deployed to $KINDLE:/mnt/us/zotero/bin"
