#pragma once
// Two-way metadata sync between the local Library and zotero.org (spec §7.4).
// Pull: collections, then items/attachments/annotations changed since the stored library
// version, then deletions; each batch of 50 is one transaction, and the library version only
// advances once everything is in. Push: annotations created on the Kindle, additive only.
// Conflicts never upload or delete anything: the Kindle's side is kept locally, marked conflict.

#include "store/library.h"
#include "zotero/api.h"

#include <functional>
#include <optional>
#include <string>

namespace zk {

struct SyncProgress {
    std::string phase;  // "collections", "items", "deletions", "upload"
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

private:
    void pullInto(SyncReport&, const std::function<void(const SyncProgress&)>&);
    void apply(const nlohmann::json& obj, SyncReport&);
    void removeKey(const std::string& key, SyncReport&);
    int conflictUnsynced(const std::string& attachmentKey);
    bool pushOnce(SyncReport&, bool& preconditionFailed);

    ZoteroApi& api_;
    Library& lib_;
};

}  // namespace zk
