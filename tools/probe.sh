#!/bin/sh
# Name: Zotero probe
# Author: zotero-for-kindle
#
# Device survey for building a native app. Read-only, except that it gives
# the USBNetwork interface its expected address if it has none (lost on reboot).
# Writes /mnt/us/documents/zk-probe.txt (readable over MTP).

OUT=${ZK_OUT:-/mnt/us/documents/zk-probe.txt}
echo "Probing device, writing $OUT ..."

sec() { echo; echo "===== $1 ====="; }
run() { echo "--- \$ $*"; sh -c "$*" 2>&1; }

{
sec "identity"
run "date"
run "cat /etc/prettyversion.txt"
run "cat /etc/version.txt"
run "uname -a"
run "cut -c1-8 /proc/usid"
run "cat /proc/cpuinfo"
run "cat /proc/meminfo | head -5"
run "df -h"

sec "abi / libc"
run "ls -l /lib/ld-linux* /lib/libc.so* /lib/libc-* /lib/libstdc++* /usr/lib/libstdc++*"
run "/lib/libc.so.6 | head -2"
run "busybox | head -1"
run "which python python3 lua luajit perl curl wget openssl sqlite3 fbink fbset lipc-get-prop lipc-set-prop eips"

sec "graphics / ui stack libs"
run "ls /usr/lib /lib | grep -i -E 'gtk|gdk|cairo|pango|glib|gobject|gio|harfbuzz|freetype|fontconfig|png|jpeg|webp|X11|Xext|xcb|Xrender|Xft|egl|gles|drm|qt|webkit|skia|wayland|lipc|curl|ssl|crypto|sqlite|z\\.so|mupdf|icu|json|blanket|ion' | sort -u"

sec "processes"
run "ps -ef 2>/dev/null || ps w"

sec "framebuffer / screen"
run "cat /proc/fb"
run "for f in /sys/class/graphics/fb0/*; do [ -f \"\$f\" ] && echo \"\$f: \$(cat \$f 2>/dev/null | head -3 | tr '\\n' ' ')\"; done"
run "fbset 2>/dev/null"
run "for b in /mnt/us/libkh/bin/fbink /var/local/kmc/bin/fbink /mnt/us/koreader/fbink /usr/bin/fbink \$(which fbink 2>/dev/null); do [ -x \"\$b\" ] && { echo \"using \$b\"; \$b -e; break; }; done"
run "ls /sys/class/backlight /sys/class/leds 2>/dev/null"

sec "input"
run "cat /proc/bus/input/devices"
run "ls -l /dev/input"

sec "fonts"
run "ls /usr/java/lib/fonts /usr/share/fonts /usr/share/fonts/* /mnt/us/fonts 2>/dev/null | head -80"

sec "lipc"
run "lipc-probe -a 2>/dev/null | head -150"
run "lipc-get-prop com.lab126.powerd status"
run "lipc-get-prop com.lab126.winmgr orientationLock"
run "lipc-get-prop com.lab126.wifid cmState"

sec "jailbreak / mods"
run "ls -la /mnt/us"
run "ls -la /mnt/us/extensions"
run "ls -la /var/local/kmc /mnt/us/libkh /var/local/mkk 2>/dev/null"
run "ls /mnt/us/documents | grep -E '\\.sh\$'"
run "ls /mnt/us/koreader 2>/dev/null | head"

sec "usbnetwork debug"
run "ls -la /mnt/us/usbnet /mnt/us/usbnet/etc /mnt/us/usbnet/bin 2>/dev/null"
run "cat /mnt/us/usbnet/etc/config 2>/dev/null | grep -v '^#' | grep -v '^\$'"
run "ls /mnt/us/extensions/*/ 2>/dev/null"
run "ifconfig -a"
run "ip addr 2>/dev/null"
run "route -n"
run "ps -ef 2>/dev/null | grep -i -E 'dropbear|sshd|usbnet|tailscale' | grep -v grep"
run "lsmod"
run "cat /sys/class/udc/*/state 2>/dev/null; ls /sys/class/udc 2>/dev/null"
run "ls -R /sys/kernel/config/usb_gadget 2>/dev/null | head -60"
run "iptables -L -n 2>/dev/null | head -40"
run "dmesg | grep -i -E 'usb|gadget|ether|rndis|ecm|mtu3' | tail -40"
run "grep -i -E 'usbnet|dropbear|usb0' /var/log/messages 2>/dev/null | tail -40"
run "tail -40 /mnt/us/usbnet/usbnetwork.log /var/tmp/usbnet*.log 2>/dev/null"
run "ls /sys/class/net"
run "netstat -ltn 2>/dev/null"

sec "usb network fix (only if the usb interface has no IPv4)"
for IF in usb0 rndis0; do
    [ -e /sys/class/net/$IF ] || continue
    if ifconfig $IF | grep -q 'inet addr'; then
        echo "$IF already has an address, leaving it alone"
    else
        echo "$IF has no IPv4, assigning 192.168.15.244"
        ifconfig $IF 192.168.15.244 netmask 255.255.255.0 up 2>&1
    fi
    run "ifconfig $IF"
done
run "iptables -I INPUT -i usb0 -j ACCEPT 2>/dev/null && echo 'usb0 accepted in firewall'"
} > "$OUT" 2>&1

sync
echo "Done: $(wc -c < "$OUT") bytes in $OUT"
