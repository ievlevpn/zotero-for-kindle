#!/bin/sh
# Copies the Kindle build to the device over USBNetLite.
#   tools/deploy.sh [extra files to put in /mnt/us/zotero/bin...]
set -eu
cd "$(dirname "$0")/.."
KINDLE=${KINDLE:-root@192.168.15.244}
ssh "$KINDLE" 'mkdir -p /mnt/us/zotero/bin /mnt/us/zotero/share /mnt/us/zotero/log'
scp -q build-kindle/zotero "$@" "$KINDLE:/mnt/us/zotero/bin/"
ssh "$KINDLE" 'chmod +x /mnt/us/zotero/bin/*'
echo "deployed to $KINDLE:/mnt/us/zotero/bin"
