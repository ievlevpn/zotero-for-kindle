#!/bin/sh
# Name: Zotero
# Author: zotero-for-kindle
# Icon: /mnt/us/zotero/share/icon.png
# DontUseFBInk
cd /mnt/us/zotero || exit 1
mkdir -p log
# Copied over USB, the programs may have lost their execute bit.
chmod +x bin/* 2>/dev/null
DISPLAY=:0 ./bin/zotero >> log/zotero.log 2>&1
# Whatever happened to the app, let the Kindle sleep again.
lipc-set-prop com.lab126.powerd preventScreenSaver 0
