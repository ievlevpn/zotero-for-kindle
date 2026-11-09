#pragma once
// Two-way metadata sync between the local Library and zotero.org (spec §7.4).
// Pull: collections, then items/attachments/annotations changed since the stored library
// version, then deletions; each batch of 50 is one transaction, and the library version only
// advances once everything is in. Push: the Kindle's annotation deletions, then its creations
// and edits (spec §7.4), reading positions, the read tag of items marked read or unread, and plugin changes. Conflicts never upload or delete anything: Zotero's copy wins and the
// Kindle's side is kept locally, marked conflict.

#include "net/write_guard.h"
#include "plugins/events.h"
#include "store/library.h"
#include "zotero/api.h"

#include <functional>
#include <optional>
#include <string>

namespace zk {

struct SyncProgress {
    std::string phase;  // "collections", "items", "annotations", "deletions"
    int done = 0, total = 0;
};

struct SyncReport {
    int itemsAdded = 0, itemsUpdated = 0, itemsRemoved = 0, annotations = 0, conflicts = 0;
    int pluginSent = 0, pluginRefused = 0;  // plugin changes that ended Sent / Refused in this push
    int64_t fromVersion = 0, toVersion = 0;
    std::optional<UserError> error;
    // What a pull changed, as plugin events (src/plugins/events.h): annotations created, changed
    // or deleted by Zotero, items marked read or unread there. Built on the sync worker's thread.
    // Empty for the first full sync of a library (`initial`), which tells plugins only that it
    // finished, and for the on-demand download of a book's annotations.
    std::vector<PluginEvent> pluginEvents;
    bool initial = false;  // this pull was the library's first full sync (fromVersion 0)
};

// The WriteGuard's view of the plugin changes pending in `lib` (its plugin rules), as Zotero has
// each key: what both WriteGuards (the app's sync service and zk-sync) are given.
WriteGuard::PluginChanges pluginChangeShapes(Library& lib);

class SyncEngine {
public:
    SyncEngine(ZoteroApi&, Library&);
    SyncReport pull(std::function<void(const SyncProgress&)> progress);
    SyncReport push();
    int pendingChanges();  // what push() would send: deletions, creations, edits, positions, read marks, plugin changes
    // On-demand annotations: called when a book is opened. Downloads the attachment's
    // annotations and keeps them in sync from then on.
    SyncReport pullAnnotationsFor(const std::string& attachmentKey, std::function<void(const SyncProgress&)> progress);
    // The "Keep all highlights offline" setting. Off drops the downloaded annotations of books
    // not opened on the Kindle (never local edits or conflicts); on syncs all at the next pull.
    void setAllHighlightsOffline(bool on);

private:
    void pullInto(SyncReport&, const std::function<void(const SyncProgress&)>&);
    // Fetches `keys` in batches of 50, one transaction per batch. `accept` filters objects.
    bool fetchAndApply(const std::string& kind, const std::string& phase, const std::vector<std::string>& keys,
                       SyncReport&, const std::function<void(const SyncProgress&)>&,
                       const std::function<bool(const nlohmann::json&)>& accept = nullptr);
    void apply(const nlohmann::json& obj, SyncReport&);
    void removeKey(const std::string& key, SyncReport&);
    int conflictUnsynced(const std::string& attachmentKey);
    bool pushOnce(SyncReport&, bool& preconditionFailed);
    bool pushDeletions(SyncReport&, bool& preconditionFailed);
    void pullPositions(SyncReport&);
    std::vector<ReadingPosition> sendablePositions();
    bool pushPositions(SyncReport&, bool& preconditionFailed);
    bool pushReadMarks(SyncReport&);
    // Plugin changes (zk.zotero.*), in dependency order: creates first (batched), then the rest,
    // each against its object's version. A refused one never holds the others back.
    bool pushPluginChanges(SyncReport&);
    bool pushPluginCreates(SyncReport&, std::vector<PluginChange>, bool& preconditionFailed);
    bool pushPluginEdit(SyncReport&, const PluginChange&);

    ZoteroApi& api_;
    Library& lib_;
    plugins::Pulled pulled_;  // this pull's changes for plugins; kept only when collecting_
    bool collecting_ = false;
    std::vector<std::string> refetch_;  // annotations to download again at the end of this pull
};

}  // namespace zk
