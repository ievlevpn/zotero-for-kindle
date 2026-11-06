#!/bin/sh
# Runs a deployed binary on the Kindle over SSH, on the device's X display.
#   tools/run-kindle.sh [binary (default: zotero)] [args...]
set -eu
KINDLE=${KINDLE:-root@192.168.15.244}
BIN=${1:-zotero}
[ $# -gt 0 ] && shift
ssh "$KINDLE" "cd /mnt/us/zotero && DISPLAY=:0 ./bin/$BIN $*" 2>&1 | grep -v -E 'post-quantum|store now, decrypt|openssh.com/pq'
