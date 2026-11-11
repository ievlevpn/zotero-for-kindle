#!/bin/sh
# Name: Zotero
# Author: zotero-for-kindle
# Icon: /mnt/us/zotero/share/icon.png
# DontUseFBInk
#
# Starts the app, and keeps its updates safe: update protocol format 1 (src/update/install.h).
# Every launcher from 0.2.0 on must understand it; a new format ships a launcher that knows both.
#   update/state   "installing|installed|tried <from> <to>", written by the app while installing
#   update/old/    bin lib share Zotero.sh zotero.db: what a rollback restores, with the account's
#                  backup beside it, $ZK_CONFIG.old (kept off the USB drive, mode 0600)
#   exit code 75   the app installed an update: start it again
# installing: the swap was interrupted. tried: the new version ran and never confirmed (the app
# confirms by deleting update/state once it has drawn its first screen). Both: roll back.
ZK_HOME=${ZK_HOME:-/mnt/us/zotero}
ZK_LAUNCHER=${ZK_LAUNCHER:-/mnt/us/documents/Zotero.sh}
ZK_CONFIG=${ZK_CONFIG:-/var/local/zotero/config.json}
ZK_DISPLAY=${ZK_DISPLAY:-:0}
ZK_INK_MARK=${ZK_INK_MARK:-/tmp/zk-wet-ink-session}  # left by the app while the Scribe's wet ink is on
ZK_PIDFILE=${ZK_PIDFILE:-/tmp/zotero.pid}  # the running app's pid; /tmp is emptied at boot
cd "$ZK_HOME" || exit 1
mkdir -p log
U=update

note() { echo "$(date '+%H:%M:%S') launcher: $*" >> log/zotero.log; }

# One app at a time: a second copy would work on the same database. The app can outlive its
# launcher (a killed upstart job, say), so the check is on the app's pid, not on a launcher's.
# Where /proc exists, a pid that now belongs to another program doesn't count.
running_pid() {
    [ -f "$ZK_PIDFILE" ] || return 1
    pid=
    read -r pid < "$ZK_PIDFILE" 2>/dev/null
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null || return 1
    if [ -r "/proc/$pid/cmdline" ]; then
        tr '\0' ' ' < "/proc/$pid/cmdline" | grep -q 'bin/zotero' || return 1
    fi
    echo "$pid"
}
if pid=$(running_pid); then
    # SIGWINCH asks the app to come to the front. An older app ignores it (the signal's default),
    # where SIGUSR1 would kill it: a new launcher can meet an old app around updates and rollbacks.
    kill -WINCH "$pid" 2>/dev/null
    note "already running (pid $pid): brought to the front, no second copy"
    exit 0
fi
mkdir -p "$(dirname "$ZK_PIDFILE")"

# Keeps the log to about 2 MB in all: over 1 MB, it becomes zotero.log.1 (replacing the old one).
# `wc -c <` is in POSIX and busybox alike; unquoted, its macOS padding goes.
rotate_log() {
    [ -f log/zotero.log ] || return 0
    size=$(wc -c < log/zotero.log 2>/dev/null) || return 0
    [ -n "$size" ] && [ $size -gt 1048576 ] && mv -f log/zotero.log log/zotero.log.1
    return 0
}

# Puts update/old back. Every step can be repeated, so an interrupted rollback finishes later.
restore() {
    for d in bin lib share; do
        if [ -d "$U/old/$d" ]; then
            rm -rf "$d" && cp -r "$U/old/$d" "$d" || return 1
        fi
    done
    if [ -f "$U/old/Zotero.sh" ]; then
        cp "$U/old/Zotero.sh" "$ZK_LAUNCHER.tmp" && mv "$ZK_LAUNCHER.tmp" "$ZK_LAUNCHER" || return 1
    fi
    if [ -f "$U/old/zotero.db" ]; then
        cp "$U/old/zotero.db" zotero.db.tmp && rm -f zotero.db-wal zotero.db-shm && mv zotero.db.tmp zotero.db || return 1
    fi
    if [ -f "$ZK_CONFIG.old" ]; then
        cp "$ZK_CONFIG.old" "$ZK_CONFIG.tmp" && chmod 600 "$ZK_CONFIG.tmp" && mv "$ZK_CONFIG.tmp" "$ZK_CONFIG" || return 1
    fi
    echo "$1 $2" > "$U/rolled-back"
    sync  # the restored files reach the disk before the state that asked for them goes
    rm -f "$U/state"
    rm -rf "$U/old" "$U/new" "$U/trash"
    rm -f "$ZK_CONFIG.old"
    note "rolled back from $2 to $1"
}

# Looks at update/state before a start: rolls back if needed, marks a first start as tried.
check_update() {
    [ -f "$U/state" ] || return 0
    word= from= to=
    read -r word from to < "$U/state"
    case "$word" in
        installing|tried) restore "$from" "$to" || note "rollback from $to failed; will try again" ;;
        installed)
            echo "tried $from $to" > "$U/state.tmp" && mv "$U/state.tmp" "$U/state"
            sync
            ;;
        *)  # unreadable (a write torn by a power cut): go back if there is something to go back to
            if [ -d "$U/old/bin" ]; then
                restore "?" "?" || note "rollback after an unreadable state failed; will try again"
            fi
            ;;
    esac
}

starts=0
while [ "$starts" -lt 5 ]; do
    starts=$((starts + 1))
    rotate_log
    check_update
    chmod +x bin/* 2>/dev/null  # copied over USB, the programs may have lost their execute bit
    # The shell writes its own pid, then becomes the app: the file is there before the app runs.
    DISPLAY=$ZK_DISPLAY sh -c 'echo $$ > "$1" && exec ./bin/zotero' sh "$ZK_PIDFILE" >> log/zotero.log 2>&1 &
    wait $!
    code=$?
    rm -f "$ZK_PIDFILE"
    [ "$code" = 75 ] && continue
    # A new version that ended before confirming: go back now, and start the old one.
    if [ -f "$U/state" ] && read -r word rest < "$U/state" && [ "$word" = tried ]; then
        note "the new version exited ($code) before it started properly"
        continue
    fi
    break
done
# An end that skipped the app's own clean-up (killed, out of memory): the Scribe's wet ink may be
# left on over a window that's gone. Turn it off, only if the app left its session open (an older
# app, after a rollback, would take the flag for a normal start).
if [ "$code" != 0 ] && [ "$code" != 75 ] && [ -e "$ZK_INK_MARK" ]; then
    DISPLAY=$ZK_DISPLAY ./bin/zotero --reset-system-ink >> log/zotero.log 2>&1
fi
# Whatever happened to the app, let the Kindle sleep again.
command -v lipc-set-prop >/dev/null 2>&1 && lipc-set-prop com.lab126.powerd preventScreenSaver 0
exit 0
