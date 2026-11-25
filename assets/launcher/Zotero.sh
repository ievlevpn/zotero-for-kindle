#!/bin/sh
# Name: Zotero
# Author: zotero-for-kindle
# Icon: /mnt/us/zotero/share/icon.png
# DontUseFBInk
#
# Starts the app, and keeps its updates safe: update protocol format 1 (src/update/install.h).
# Every launcher from 0.2.0 on must understand it; a new format ships a launcher that knows both.
#   update/state   "installing|installed|tried <from> <to>", written by the app while installing
#   update/old/    bin lib share Zotero.sh, and the libraries: what a rollback restores.
#                  accounts/<userId>/zotero.db: each account's (0.13 on), with the account list's
#                  backup beside it, $ZK_ACCOUNTS.old; zotero.db: the one library of a version
#                  before accounts, with its account's backup beside it, $ZK_CONFIG.old (both
#                  kept off the USB drive, mode 0600)
#   exit code 75   the app installed an update: start it again
# installing: the swap was interrupted. tried: the new version ran and never confirmed (the app
# confirms by deleting update/state after 60 s in front, or at a clean exit). Both: roll back.
ZK_HOME=${ZK_HOME:-/mnt/us/zotero}
ZK_LAUNCHER=${ZK_LAUNCHER:-/mnt/us/documents/Zotero.sh}
ZK_CONFIG=${ZK_CONFIG:-/var/local/zotero/config.json}
ZK_ACCOUNTS=${ZK_ACCOUNTS:-$(dirname "$ZK_CONFIG")/accounts.json}
ZK_DISPLAY=${ZK_DISPLAY:-:0}
ZK_INK_MARK=${ZK_INK_MARK:-/tmp/zk-wet-ink-session}  # left by the app while the Scribe's wet ink is on
ZK_PIDFILE=${ZK_PIDFILE:-/tmp/zotero.pid}  # the running app's pid; /tmp is emptied at boot
ZK_LOCK=${ZK_LOCK:-/tmp/zotero.lock}       # a link to "<pid> <file name>" of the launcher that runs the app
ZK_PROC=${ZK_PROC:-/proc}
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
    if [ -r "$ZK_PROC/$pid/cmdline" ]; then
        tr '\0' ' ' < "$ZK_PROC/$pid/cmdline" | grep -q 'bin/zotero' || return 1
    fi
    echo "$pid"
}
# One launcher at a time, for its whole loop: between two runs of the app (a restart, a rollback)
# there is no app pid to find, and a second launcher there would start a second copy over a
# half-restored install. The lock is a symlink naming the holder's pid and file name, made in one
# step. One whose launcher is gone (a power cut, a kill) is taken over: where /proc shows that pid
# running something that isn't the file it named, the pid was reused. The name is the holder's own
# ($0), whatever the scriptlet runner called it; with none recorded, a live pid holds.
launcher_alive() {
    kill -0 "$1" 2>/dev/null || return 1
    if [ -n "$2" ] && [ -r "$ZK_PROC/$1/cmdline" ]; then
        tr '\0' ' ' < "$ZK_PROC/$1/cmdline" | grep -qF "$2" || return 1
    fi
}
# After SIGWINCH (come to the front), this launcher stays until that app ends, as the one that
# started it does: the Kindle's library takes a scriptlet that has ended for an app that has closed
# and comes back to the front, over the window just raised (2026-11-25: a second tap showed the app
# for a second, then the Kindle home).
stay_while_running() {
    while [ "$(running_pid)" = "$1" ]; do sleep 1; done
}
me="$$ $(basename "$0")"
take_lock() {
    mkdir -p "$(dirname "$ZK_LOCK")"
    ln -s "$me" "$ZK_LOCK" 2>/dev/null && return 0
    link=$(readlink "$ZK_LOCK" 2>/dev/null)
    holder=${link%% *}
    name=
    [ "$link" != "$holder" ] && name=${link#* }
    [ -n "$holder" ] && launcher_alive "$holder" "$name" && return 1
    rm -f "$ZK_LOCK"
    ln -s "$me" "$ZK_LOCK" 2>/dev/null
}
if ! take_lock; then
    if pid=$(running_pid); then
        kill -WINCH "$pid" 2>/dev/null
        note "already running (pid $pid): brought to the front, no second copy"
        stay_while_running "$pid"
    else
        note "another launcher (pid $holder) is starting the app: no second copy"
    fi
    exit 0
fi
# At every end of this launcher, a kill included: the lock goes, and once an app has been started
# the Kindle may sleep again (the app's keep-awake, which its own signal path can't turn off).
started=
finish() {
    if [ -n "$started" ]; then
        command -v lipc-set-prop >/dev/null 2>&1 && lipc-set-prop com.lab126.powerd preventScreenSaver 0
    fi
    [ "$(readlink "$ZK_LOCK" 2>/dev/null)" = "$me" ] && rm -f "$ZK_LOCK"
    return 0
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if pid=$(running_pid); then
    # SIGWINCH asks the app to come to the front. An older app ignores it (the signal's default),
    # where SIGUSR1 would kill it: a new launcher can meet an old app around updates and rollbacks.
    kill -WINCH "$pid" 2>/dev/null
    note "already running (pid $pid): brought to the front, no second copy"
    stay_while_running "$pid"
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

# A file or folder moved back to the old version's place, unless one is there already.
move_back() {
    if [ -e "$1" ] && [ ! -e "$2" ]; then
        mkdir -p "$(dirname "$2")" && mv "$1" "$2" || return 1
    fi
}

# Going back to a version from before accounts (its backup holds the library in the old place and
# no account list): what the new version moved into its account's folder goes back where the old
# one keeps it (the downloads, each plugin's data), and the moved library is put aside (never
# deleted), with the account list gone, so the next update moves everything again from the library
# restored here.
unmove_account() {
    id=$(sed -n 's/.*"userId"[^0-9]*\([0-9][0-9]*\).*/\1/p' "$ZK_CONFIG" 2>/dev/null | head -n 1)
    a=accounts/$id
    if [ -n "$id" ] && [ -d "$a" ]; then
        aside=accounts/.leftovers/rollback-$(date +%Y%m%d-%H%M%S)/$id
        for f in zotero.db zotero.db-wal zotero.db-shm zotero.db-journal; do
            move_back "$a/$f" "$aside/$f" || return 1
        done
        move_back "$a/files" files || return 1
        for p in "$a"/plugins/*; do
            pid=$(basename "$p")
            [ -d "plugins/$pid" ] || continue  # removed since: its data stays in the account
            for f in storage.db storage.db-wal storage.db-shm storage.db-journal data; do
                move_back "$p/$f" "plugins/$pid/$f" || return 1
            done
        done
    fi
    rm -f accounts/.migrating-* "$ZK_ACCOUNTS"
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
    for b in "$U"/old/accounts/*/zotero.db; do
        [ -f "$b" ] || continue
        a=accounts/$(basename "$(dirname "$b")")
        mkdir -p "$a" && cp "$b" "$a/zotero.db.tmp" && rm -f "$a/zotero.db-wal" "$a/zotero.db-shm" &&
            mv "$a/zotero.db.tmp" "$a/zotero.db" || return 1
    done
    if [ -f "$ZK_ACCOUNTS.old" ]; then
        cp "$ZK_ACCOUNTS.old" "$ZK_ACCOUNTS.tmp" && chmod 600 "$ZK_ACCOUNTS.tmp" && mv "$ZK_ACCOUNTS.tmp" "$ZK_ACCOUNTS" || return 1
    fi
    if [ -f "$U/old/zotero.db" ]; then
        cp "$U/old/zotero.db" zotero.db.tmp && rm -f zotero.db-wal zotero.db-shm && mv zotero.db.tmp zotero.db || return 1
    fi
    if [ -f "$ZK_CONFIG.old" ]; then
        cp "$ZK_CONFIG.old" "$ZK_CONFIG.tmp" && chmod 600 "$ZK_CONFIG.tmp" && mv "$ZK_CONFIG.tmp" "$ZK_CONFIG" || return 1
    fi
    if [ -f "$U/old/zotero.db" ] && [ ! -f "$ZK_ACCOUNTS.old" ]; then  # the old version had no accounts
        unmove_account || return 1
    fi
    echo "$1 $2" > "$U/rolled-back"
    sync  # the restored files reach the disk before the state that asked for them goes
    rm -f "$U/state"
    rm -rf "$U/old" "$U/new" "$U/trash"
    rm -f "$ZK_CONFIG.old" "$ZK_ACCOUNTS.old"
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
started=1
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
    # How it ended, for the log: 139 a crash (SIGSEGV), 134 an abort, 137 killed (often out of
    # memory), 3 the app's own stall exit; 0 and 75 are normal.
    case "$code" in 0|75) ;; *) note "the app ended with code $code" ;; esac
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
exit 0  # finish: whatever happened to the app, the Kindle may sleep again
