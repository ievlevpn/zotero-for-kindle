# Zotero for Kindle

Native Zotero client and reader for a jailbroken Kindle Paperwhite 5.
Design: `docs/superpowers/specs/2026-11-06-zotero-kindle-design.md`.

## Desktop build (macOS)

    meson setup build-desktop
    meson test -C build-desktop
    ./build-desktop/zotero

## Kindle build and deploy

    tools/build-kindle.sh      # cross-build in the zk-kindlehf container
    tools/deploy.sh            # copy to root@192.168.15.244:/mnt/us/zotero
    tools/run-kindle.sh        # run on the device over SSH

Docker runs in a colima VM stored on the external drive (it needs `/Volumes/wooster`):

    COLIMA_HOME=/Volumes/wooster/colima colima start

The Kindle must be in USBNetwork mode (KUAL → USBNetLite, toggle with the cable unplugged).

## Licence

AGPL-3.0-or-later.
