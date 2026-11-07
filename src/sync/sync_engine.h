#pragma once
// Two-way metadata sync between the local Library and zotero.org (spec §7.4).
// Pull: collections, then items/attachments/annotations changed since the stored library
// version, then deletions; each batch of 50 is one transaction, and the library version only
// advances once everything is in. Push: the Kindle's annotation deletions, then its creations
// and edits (spec §7.4). Conflicts never upload or delete anything: Zotero's copy wins and the
// Kindle's side is kept locally, marked conflict.

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
    int64_t fromVersion = 0, toVersion = 0;
    std::optional<UserError> error;
};

class SyncEngine {
public:
    SyncEngine(ZoteroApi&, Library&);
    SyncReport pull(std::function<void(const SyncProgress&)> progress);
    SyncReport push();
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

    ZoteroApi& api_;
    Library& lib_;
};

}  // namespace zk
