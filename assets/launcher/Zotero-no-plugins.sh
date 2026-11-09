#!/bin/sh
# Name: Zotero (no plugins)
# Author: zotero-for-kindle
# Icon: /mnt/us/zotero/share/icon.png
# DontUseFBInk
#
# Starts Zotero with every plugin off, for this run only: if a plugin keeps the app from working,
# open this, then Settings -> Plugins, and turn the plugin off or remove it. All the starting and
# update logic stays in Zotero.sh.
ZK_NO_PLUGINS=1
export ZK_NO_PLUGINS
exec "${ZK_LAUNCHER:-/mnt/us/documents/Zotero.sh}" "$@"
