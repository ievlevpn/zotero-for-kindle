#pragma once
// Putting a verified release in place, so that a failed update can always be undone.
//
// installRelease copies the current bin/lib/share, the launcher, the library database and the
// account file into <appDir>/update/old/, writes update/state = "installing <from> <to>", swaps
// the new folders in (current ones to update/trash/), then writes "installed <from> <to>". The
// app then exits with 75 and the launcher (assets/launcher/Zotero.sh, protocol format 1) starts
// the new version, marking it "tried"; if it never confirms, the launcher restores update/old/.
// confirmStartup is that confirmation.

#include "net/errors.h"
#include "update/version.h"

#include <functional>
#include <string>

namespace zk {

struct InstallPaths {
    std::string appDir;        // /mnt/us/zotero; empty = can't install (the desktop)
    std::string launcherPath;  // /mnt/us/documents/Zotero.sh
    std::string configFile;    // /var/local/zotero/config.json
    std::string dbPath;        // /mnt/us/zotero/zotero.db
    std::string updateDir() const { return appDir + "/update"; }
};

// The zip's zotero/* into newDir/*, documents/Zotero.sh as newDir/Zotero.sh; programs made
// executable; refused (and newDir removed) if an entry escapes or a needed file is missing.
bool unpackRelease(const std::string& zipPath, const std::string& newDir, UserError*);

// `beat` is called between steps (the stall watchdog). Fails without changing the install if
// anything goes wrong before the swap; a failure during the swap leaves state "installing", which
// the launcher undoes at the next start.
bool installRelease(const InstallPaths&, const std::string& newDir, Version from, Version to,
                    std::function<void(const char* step)> beat, UserError*);

enum class StartupNote { None, Updated, RolledBack };
struct Confirmed {
    StartupNote note = StartupNote::None;
    Version from, to;
};

// Called once the app has drawn its first screen. Updated: this is the new version's first good
// start (the backups are deleted). RolledBack: the launcher went back after a failed update.
Confirmed confirmStartup(const InstallPaths&, Version running);
Confirmed confirmStartup(const InstallPaths&);  // running = appVersion()

}  // namespace zk
