#include "sync/sync_engine.h"

#include "core/log.h"
#include "zotero/parse.h"

#include <random>

namespace zk {

using nlohmann::json;

namespace {

constexpr size_t kBatch = 50;

// A fresh local-only key in Zotero's alphabet, for the Kindle's copy of a conflicted annotation.
std::string newLocalKey() {
    static std::mt19937 rng{std::random_device{}()};
    static const char* alphabet = "23456789ABCDEFGHIJKLMNPQRSTUVWXYZ";
    std::string k(8, '2');
    for (auto& c : k) c = alphabet[rng() % 33];
    return k;
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
    if (a.version > 0) {
        j["key"] = a.key;
        j["version"] = a.version;
    }
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
        lib_.trackAttachment(attachmentKey);
        UserError err;
        auto listing = api_.annotationVersions(attachmentKey, 0, &err);
        if (!listing) {
            rep.error = err;
        } else {
            std::vector<std::string> changed;
            for (const auto& [key, version] : listing->versions)
                if (lib_.objectVersion(key) < version) changed.push_back(key);
            fetchAndApply("items", "annotations", changed, rep, progress ? progress : [](const SyncProgress&) {});
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
            break;
        }
        case ObjectKind::Annotation: {
            AnnotationRecord incoming = parseAnnotation(obj);
            if (auto local = lib_.annotation(key)) {
                incoming.createdHere = local->createdHere;
                if (local->dirty && version > local->version) {
                    // Edited on both sides: Zotero's version wins, the Kindle edit is kept as a conflict.
                    AnnotationRecord mine = *local;
                    mine.key = newLocalKey();
                    mine.version = 0;
                    mine.dirty = false;
                    mine.conflict = true;
                    mine.createdHere = true;
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
        a.dirty = false;
        a.conflict = true;
        lib_.upsertAnnotation(a);
        ++n;
    }
    return n;
}

void SyncEngine::removeKey(const std::string& key, SyncReport& rep) {
    if (auto a = lib_.annotation(key)) {
        if (a->dirty) {
            // Deleted on the desktop, edited here: the deletion stands on the server; the Kindle
            // edit stays as a local conflict (re-applying it later creates a new annotation).
            a->dirty = false;
            a->conflict = true;
            a->version = 0;
            lib_.upsertAnnotation(*a);
            lib_.setObjectVersion(key, 0);
            ++rep.conflicts;
        } else if (!a->conflict) {
            lib_.removeObject("annotation", key);
        }
        return;
    }
    if (lib_.attachment(key)) {
        rep.conflicts += conflictUnsynced(key);
        for (const auto& a : lib_.annotationsFor(key))
            if (!a.conflict) lib_.removeObject("annotation", a.key);
        lib_.removeObject("attachment", key);
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
            // The library changed on the server: pull first, then send our additions again.
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

// Sends every unsent annotation created on the Kindle. False if a batch hit a 412 or failed.
bool SyncEngine::pushOnce(SyncReport& rep, bool& preconditionFailed) {
    preconditionFailed = false;
    std::vector<AnnotationRecord> pending;
    for (auto& a : lib_.dirtyAnnotations())
        if (a.createdHere && !a.conflict) pending.push_back(std::move(a));
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
            AnnotationRecord a = pending[i + index];
            if (a.key != key) lib_.removeObject("annotation", a.key);
            a.key = key;
            a.version = res->libraryVersion;
            a.dirty = false;
            lib_.upsertAnnotation(a);
            lib_.setObjectVersion(key, res->libraryVersion);
            ++rep.annotations;
        }
        // Our write was the only change since the version we sent (otherwise: 412).
        lib_.setLibraryVersion(res->libraryVersion);
        tx.commit();
        if (!res->failed.empty()) {
            rep.error = UserError{ErrorKind::Other,
                                  "Zotero refused " + std::to_string(res->failed.size()) + " annotation(s).",
                                  res->failed.begin()->second};
            return false;
        }
    }
    return true;
}

}  // namespace zk
