#include "sync/sync_engine.h"

#include "core/log.h"
#include "zotero/parse.h"

#include <random>

namespace zk {

using nlohmann::json;

namespace {

constexpr size_t kBatch = 50;

bool sameContent(const AnnotationRecord& a, const AnnotationRecord& b) {
    return a.type == b.type && a.text == b.text && a.comment == b.comment && a.color == b.color &&
           a.position == b.position && a.sortIndex == b.sortIndex && a.tags == b.tags;
}

bool trashed(const json& obj) {
    const json& d = obj.contains("data") ? obj["data"] : json::object();
    if (!d.is_object() || !d.contains("deleted")) return false;
    const json& v = d["deleted"];
    return (v.is_boolean() && v.get<bool>()) || (v.is_number() && v.get<double>() != 0);
}

int64_t versionOf(const json& obj) {
    return obj.contains("version") && obj["version"].is_number_integer() ? obj["version"].get<int64_t>() : 0;
}

json annotationJson(const AnnotationRecord& a) {
    json tags = json::array();
    for (const auto& t : a.tags) tags.push_back({{"tag", t}});
    json j = {{"itemType", "annotation"},         {"parentItem", a.parentKey},     {"annotationType", a.type},
              {"annotationComment", a.comment},   {"annotationColor", a.color},    {"annotationPageLabel", a.pageLabel},
              {"annotationSortIndex", a.sortIndex}, {"annotationPosition", a.position}, {"tags", tags}};
    if (a.type == "highlight" || a.type == "underline") j["annotationText"] = a.text;
    j["key"] = a.key;  // client-made for new ones, so a lost response can't create a duplicate
    if (a.version > 0) j["version"] = a.version;
    return j;
}

}  // namespace

SyncEngine::SyncEngine(ZoteroApi& api, Library& lib) : api_(api), lib_(lib) {}

SyncReport SyncEngine::pull(std::function<void(const SyncProgress&)> progress) {
    SyncReport rep;
    try {
        pullInto(rep, progress ? progress : [](const SyncProgress&) {});
    } catch (const DbError&) {
        rep.error = lib_.lastError();
    }
    if (rep.error) log(LogLevel::Warn, "sync: pull stopped: %s (%s)", rep.error->message.c_str(), rep.error->details.c_str());
    return rep;
}

void SyncEngine::pullInto(SyncReport& rep, const std::function<void(const SyncProgress&)>& progress) {
    const int64_t from = lib_.libraryVersion();
    rep.fromVersion = from;
    UserError err;

    // The first listing fixes the version this pull brings us to. Anything changed later has a
    // higher version and is listed again next time; objects already stored at their listed
    // version are skipped, which is what makes an interrupted first sync resumable.
    // Items and attachments before annotations, so the library is usable before annotations
    // are in. Annotations: all of them with "Keep all highlights offline" (listed since the
    // version the full annotation sync reached), otherwise only changes to tracked books.
    const bool allOffline = lib_.allAnnotationsOffline();
    struct Phase {
        const char* name;
        const char* kind;
        const char* itemType;
        int64_t since;
    };
    std::vector<Phase> phases = {{"collections", "collections", "", from}, {"items", "items", "-annotation", from}};
    if (allOffline) phases.push_back({"annotations", "items", "annotation", lib_.annotationsVersion()});
    else if (from > 0) phases.push_back({"annotations", "items", "annotation", from});
    for (const Phase& phase : phases) {
        const std::string kind = phase.kind;
        auto listing = api_.versions(kind, phase.since, &err, phase.itemType);
        if (!listing) return void(rep.error = err);
        if (kind == "collections") rep.toVersion = listing->libraryVersion;
        std::vector<std::string> changed;
        for (const auto& [key, version] : listing->versions)
            if (lib_.objectVersion(key) < version) changed.push_back(key);
        std::function<bool(const json&)> accept;
        if (std::string(phase.name) == "annotations" && !allOffline)
            accept = [&](const json& obj) {
                const json& d = obj.contains("data") ? obj["data"] : json::object();
                const std::string parent = d.is_object() ? d.value("parentItem", "") : "";
                return lib_.isTracked(parent) || lib_.annotation(obj.value("key", "")).has_value();
            };
        if (!fetchAndApply(kind, phase.name, changed, rep, progress, accept)) return;
    }

    if (!refetch_.empty()) {  // Zotero's copies of annotations whose Kindle edits became conflicts
        std::vector<std::string> keys;
        keys.swap(refetch_);
        if (!fetchAndApply("items", "annotations", keys, rep, progress)) return;
    }
    auto gone = api_.deleted(from, &err);
    if (!gone) return void(rep.error = err);
    progress({"deletions", 0, static_cast<int>(gone->items.size() + gone->collections.size())});
    {
        auto tx = lib_.begin();
        for (const auto& key : gone->items) removeKey(key, rep);
        for (const auto& key : gone->collections) lib_.removeObject("collection", key);
        tx.commit();
    }
    progress({"deletions", static_cast<int>(gone->items.size() + gone->collections.size()),
              static_cast<int>(gone->items.size() + gone->collections.size())});
    lib_.setLibraryVersion(rep.toVersion);
    if (allOffline) lib_.setAnnotationsVersion(rep.toVersion);
}

bool SyncEngine::fetchAndApply(const std::string& kind, const std::string& phase, const std::vector<std::string>& keys,
                               SyncReport& rep, const std::function<void(const SyncProgress&)>& progress,
                               const std::function<bool(const json&)>& accept) {
    UserError err;
    progress({phase, 0, static_cast<int>(keys.size())});
    for (size_t i = 0; i < keys.size(); i += kBatch) {
        std::vector<std::string> batch(keys.begin() + i, keys.begin() + std::min(keys.size(), i + kBatch));
        auto objects = api_.fetch(kind, batch, &err);
        if (!objects) {
            rep.error = err;
            return false;
        }
        auto tx = lib_.begin();
        for (const auto& obj : *objects) {
            if (accept && !accept(obj)) continue;
            if (kind == "collections") {
                if (trashed(obj)) lib_.removeObject("collection", parseCollection(obj).key);
                else lib_.upsertCollection(parseCollection(obj));
                lib_.setObjectVersion(obj.value("key", ""), versionOf(obj));
            } else {
                apply(obj, rep);
            }
        }
        tx.commit();
        progress({phase, static_cast<int>(std::min(keys.size(), i + kBatch)), static_cast<int>(keys.size())});
    }
    return true;
}

SyncReport SyncEngine::pullAnnotationsFor(const std::string& attachmentKey,
                                          std::function<void(const SyncProgress&)> progress) {
    SyncReport rep;
    rep.fromVersion = rep.toVersion = lib_.libraryVersion();
    try {
        UserError err;
        auto listing = api_.annotationVersions(attachmentKey, 0, &err);
        if (!listing) {
            rep.error = err;
        } else {
            std::vector<std::string> changed;
            for (const auto& [key, version] : listing->versions)
                if (lib_.objectVersion(key) < version) changed.push_back(key);
            // Tracked only once complete: regular pulls only bring later changes, so a book
            // tracked after a partial download would stay incomplete for good.
            if (fetchAndApply("items", "annotations", changed, rep, progress ? progress : [](const SyncProgress&) {}))
                lib_.trackAttachment(attachmentKey);
        }
    } catch (const DbError&) {
        rep.error = lib_.lastError();
    }
    if (rep.error) log(LogLevel::Warn, "sync: annotations for %s: %s", attachmentKey.c_str(), rep.error->message.c_str());
    return rep;
}

void SyncEngine::setAllHighlightsOffline(bool on) {
    auto tx = lib_.begin();
    lib_.setAllAnnotationsOffline(on);
    if (!on) lib_.dropUntrackedAnnotations();
    tx.commit();
}

void SyncEngine::apply(const json& obj, SyncReport& rep) {
    const ObjectKind kind = kindOf(obj);
    const std::string key = obj.value("key", "");
    const int64_t version = versionOf(obj);
    if (key.empty()) return;
    if (trashed(obj)) {
        removeKey(key, rep);
        lib_.setObjectVersion(key, version);
        return;
    }
    switch (kind) {
        case ObjectKind::Item: {
            const bool known = lib_.objectVersion(key) > 0;
            lib_.upsertItem(parseItem(obj));
            ++(known ? rep.itemsUpdated : rep.itemsAdded);
            break;
        }
        case ObjectKind::Attachment: {
            AttachmentRecord incoming = parseAttachment(obj);
            if (auto old = lib_.attachment(key)) {
                if (old->md5 == incoming.md5) {
                    incoming.localPath = old->localPath;
                    incoming.state = old->state;
                } else if (!old->md5.empty()) {
                    // The file was replaced: positions are never carried over automatically.
                    rep.conflicts += conflictUnsynced(key);
                }
            }
            lib_.upsertAttachment(incoming);
            if (incoming.parentKey.empty()) {
                // A file without a parent item is a top-level entry in Zotero: list and index it.
                const bool known = lib_.objectVersion(key) > 0;
                lib_.upsertItem(parseItem(obj));
                ++(known ? rep.itemsUpdated : rep.itemsAdded);
            }
            break;
        }
        case ObjectKind::Annotation: {
            AnnotationRecord incoming = parseAnnotation(obj);
            if (auto local = lib_.annotation(key)) {
                incoming.createdHere = local->createdHere;
                if (local->deletedHere && version <= local->version) break;  // our DELETE is still to be sent
                // (a newer server version over a tombstone: edited on the desktop, so the Kindle
                // deletion is dropped and the annotation comes back with that edit — below)
                if (local->deletedHere) {
                    lib_.upsertAnnotation(incoming);
                    ++rep.annotations;
                    break;
                }
                if (local->createdHere && local->dirty && local->version == 0) {
                    // Our own upload landed although its answer was lost: it is not a conflict.
                    if (sameContent(*local, incoming)) {
                        lib_.upsertAnnotation(incoming);
                    } else {  // edited again since: keep the edit, now based on the server version
                        AnnotationRecord mine = *local;
                        mine.version = version;
                        lib_.upsertAnnotation(mine);
                    }
                    ++rep.annotations;
                    break;
                }
                if (local->dirty && version > local->version) {
                    // Edited on both sides: Zotero's version wins, the Kindle edit is kept as a conflict.
                    AnnotationRecord mine = *local;
                    mine.key = newLocalKey();
                    mine.version = 0;
                    mine.dirty = false;
                    mine.conflict = true;
                    mine.createdHere = true;
                    mine.conflictOf = key;
                    lib_.upsertAnnotation(mine);
                    ++rep.conflicts;
                } else if (local->dirty) {
                    break;  // our unsent edit is newer than what the server listed
                }
            }
            lib_.upsertAnnotation(incoming);
            ++rep.annotations;
            break;
        }
        case ObjectKind::Note:
        case ObjectKind::Other:
            break;  // notes are not shown in v1
    }
    lib_.setObjectVersion(key, version);
}

int SyncEngine::conflictUnsynced(const std::string& attachmentKey) {
    int n = 0;
    for (auto a : lib_.annotationsFor(attachmentKey)) {
        if (!a.dirty || a.conflict) continue;
        if (a.version > 0) {
            // An edit of an annotation that is on Zotero: the edit becomes a conflict copy of its
            // own, and Zotero's copy is downloaded again at the end of the pull.
            AnnotationRecord mine = a;
            mine.key = newLocalKey();
            mine.version = 0;
            mine.dirty = false;
            mine.conflict = true;
            mine.createdHere = true;
            mine.conflictOf = a.key;
            lib_.upsertAnnotation(mine);
            lib_.removeObject("annotation", a.key);
            lib_.setObjectVersion(a.key, 0);
            refetch_.push_back(a.key);
        } else {
            a.dirty = false;
            a.conflict = true;
            lib_.upsertAnnotation(a);
        }
        ++n;
    }
    return n;
}

void SyncEngine::removeKey(const std::string& key, SyncReport& rep) {
    if (auto a = lib_.annotation(key)) {
        if (a->deletedHere) {
            lib_.removeObject("annotation", key);  // deleted on both sides
        } else if (a->dirty) {
            // Deleted on the desktop, edited here: the deletion stands on the server; the Kindle
            // edit stays as a local conflict under a key of its own, so the server's key coming
            // back (restored from the trash) can't overwrite it.
            AnnotationRecord mine = *a;
            mine.key = newLocalKey();
            mine.dirty = false;
            mine.conflict = true;
            mine.createdHere = true;
            mine.version = 0;
            mine.conflictOf = key;
            lib_.removeObject("annotation", key);
            lib_.upsertAnnotation(mine);
            ++rep.conflicts;
        } else if (!a->conflict) {
            lib_.removeObject("annotation", key);
        }
        return;
    }
    if (auto att = lib_.attachment(key)) {
        rep.conflicts += conflictUnsynced(key);
        refetch_.clear();  // the file is gone from Zotero, and its annotations with it
        for (const auto& a : lib_.annotationsFor(key))
            if (!a.conflict) lib_.removeObject("annotation", a.key);
        for (const auto& a : lib_.pendingDeletions())  // Zotero deleted them with the file
            if (a.parentKey == key) lib_.removeObject("annotation", a.key);
        lib_.removeObject("attachment", key);
        if (att->parentKey.empty()) {  // a standalone file's list entry
            lib_.removeObject("item", key);
            ++rep.itemsRemoved;
        }
        return;
    }
    if (lib_.objectVersion(key) > 0) ++rep.itemsRemoved;  // notes count too; they share the key space
    lib_.removeObject("item", key);
}

SyncReport SyncEngine::push() {
    SyncReport rep;
    rep.fromVersion = lib_.libraryVersion();
    try {
        bool precondition = false;
        if (!pushOnce(rep, precondition) && precondition && !rep.error) {
            // The library changed on the server: pull first, then send our changes again.
            SyncReport pulled = pull(nullptr);
            rep.conflicts += pulled.conflicts;
            if (pulled.error) rep.error = pulled.error;
            else if (!pushOnce(rep, precondition) && precondition)
                rep.error = UserError{ErrorKind::Other, "Zotero kept changing during the upload. Try again.", "HTTP 412 twice"};
        }
    } catch (const DbError&) {
        rep.error = lib_.lastError();
    }
    rep.toVersion = lib_.libraryVersion();
    if (rep.error) log(LogLevel::Warn, "sync: push stopped: %s (%s)", rep.error->message.c_str(), rep.error->details.c_str());
    return rep;
}

int SyncEngine::pendingChanges() {
    int n = static_cast<int>(lib_.pendingDeletions().size());
    for (const auto& a : lib_.dirtyAnnotations()) n += !a.conflict && !a.localOnly;
    return n;
}

// Sends the Kindle's deletions, then every unsent creation and edit (conflict copies and
// annotations that can't be placed exactly stay local). False if a batch hit a 412 or failed.
bool SyncEngine::pushOnce(SyncReport& rep, bool& preconditionFailed) {
    preconditionFailed = false;
    if (!pushDeletions(rep, preconditionFailed)) return false;
    std::vector<AnnotationRecord> pending;
    for (auto& a : lib_.dirtyAnnotations())
        if (!a.conflict && !a.localOnly) pending.push_back(std::move(a));
    {
        // New annotations are sent with a key made here (committed before sending), so if the
        // answer is lost, the next pull recognises the object as our own write.
        auto tx = lib_.begin();
        for (auto& a : pending) {
            if (a.version > 0 || isZoteroKey(a.key)) continue;
            lib_.removeObject("annotation", a.key);
            a.key = newLocalKey();
            lib_.upsertAnnotation(a);
        }
        tx.commit();
    }
    for (size_t i = 0; i < pending.size(); i += kBatch) {
        const size_t end = std::min(pending.size(), i + kBatch);
        json batch = json::array();
        for (size_t j = i; j < end; ++j) batch.push_back(annotationJson(pending[j]));
        UserError err;
        auto res = api_.createItems(batch, lib_.libraryVersion(), &err);
        if (!res) {
            rep.error = err;
            return false;
        }
        if (res->preconditionFailed) {
            preconditionFailed = true;
            return false;
        }
        auto tx = lib_.begin();
        for (const auto& [index, key] : res->successKeys) {
            if (index < 0 || static_cast<size_t>(index) >= end - i) continue;
            const AnnotationRecord& sent = pending[i + index];
            // The reader may have changed or deleted it while the upload was on its way (its
            // own connection): only what was sent is marked sent.
            auto now = lib_.annotation(sent.key);
            if (sent.key != key) lib_.removeObject("annotation", sent.key);
            AnnotationRecord a = now ? *now : sent;
            a.key = key;
            a.version = res->libraryVersion;
            if (!now) {
                a.deletedHere = a.dirty = true;  // deleted meanwhile: it exists on Zotero now, so send a DELETE
            } else {
                a.dirty = a.deletedHere || !sameContent(*now, sent);
            }
            lib_.upsertAnnotation(a);
            lib_.setObjectVersion(key, res->libraryVersion);
            ++rep.annotations;
        }
        // Our write was the only change since the version we sent (otherwise: 412).
        lib_.setLibraryVersion(res->libraryVersion);
        // Objects Zotero refused: a 412 means it changed there — fetch it, so the usual rule
        // (Zotero's copy wins, ours becomes a conflict) applies; anything else would fail
        // forever, so it stays on the Kindle only, with the reason in the report.
        std::vector<std::string> changedThere;
        std::string refusal;
        for (const auto& [index, message] : res->failed) {
            if (index < 0 || static_cast<size_t>(index) >= end - i) continue;
            const AnnotationRecord& sent = pending[i + index];
            auto code = res->failedCodes.find(index);
            if (code != res->failedCodes.end() && code->second == 412) {
                changedThere.push_back(sent.key);
                continue;
            }
            if (auto now = lib_.annotation(sent.key)) {
                now->localOnly = true;
                lib_.upsertAnnotation(*now);
            }
            if (refusal.empty()) refusal = message;
        }
        tx.commit();
        if (!changedThere.empty()) {
            UserError err2;
            auto objects = api_.fetch("items", changedThere, &err2);
            if (!objects) {
                rep.error = err2;
                return false;
            }
            auto tx2 = lib_.begin();
            for (const auto& obj : *objects) apply(obj, rep);
            tx2.commit();
        }
        if (!refusal.empty()) {
            rep.error = UserError{ErrorKind::Other, "Zotero refused an annotation; it is kept on the Kindle only.", refusal};
            return false;
        }
    }
    return true;
}

bool SyncEngine::pushDeletions(SyncReport& rep, bool& preconditionFailed) {
    std::vector<std::string> keys;
    for (const auto& a : lib_.pendingDeletions()) keys.push_back(a.key);
    for (size_t i = 0; i < keys.size(); i += kBatch) {
        std::vector<std::string> batch(keys.begin() + i, keys.begin() + std::min(keys.size(), i + kBatch));
        UserError err;
        auto res = api_.deleteItems(batch, lib_.libraryVersion(), &err);
        if (!res) {
            rep.error = err;
            return false;
        }
        if (res->preconditionFailed) {
            preconditionFailed = true;
            return false;
        }
        auto tx = lib_.begin();
        for (const auto& k : batch) {
            lib_.removeObject("annotation", k);
            lib_.setObjectVersion(k, res->libraryVersion);
            ++rep.annotations;
        }
        lib_.setLibraryVersion(res->libraryVersion);  // our deletion was the only change since (else 412)
        tx.commit();
    }
    return true;
}

}  // namespace zk
