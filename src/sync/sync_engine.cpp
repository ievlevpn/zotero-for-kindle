#include "sync/sync_engine.h"

#include "core/log.h"
#include "zotero/parse.h"

#include <chrono>
#include <random>
#include <set>

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

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool deletedIn(const json& data) {
    if (!data.is_object() || !data.contains("deleted")) return false;
    const json& v = data["deleted"];
    return (v.is_boolean() && v.get<bool>()) || (v.is_number() && v.get<double>() != 0);
}

constexpr const char* kDeletedInZotero = "It was deleted in Zotero.";
constexpr const char* kNotAllowed = "The app doesn't allow this change.";

}  // namespace

WriteGuard::PluginChanges pluginChangeShapes(Library& lib) {
    return [&lib](const std::string& key) -> std::optional<PluginChangeShape> {
        PluginChangeShape shape;
        for (auto& c : lib.pendingPluginChanges())
            if (c.itemKey == key) shape.pending.push_back(std::move(c));
        for (const auto& m : lib.readMarks())
            if (m.itemKey == key) shape.readMarks.emplace_back(m.tag, m.read);
        if (shape.pending.empty() && shape.readMarks.empty()) return std::nullopt;
        shape.version = lib.objectVersion(key);
        if (auto item = lib.zoteroItem(key)) {  // as Zotero has it, without pending changes
            shape.item = true;
            if (item->version > 0) shape.version = item->version;
            shape.data = item->json;
            shape.collections = item->collections;
        } else if (auto note = lib.zoteroNote(key)) {  // one being deleted too
            shape.note = true;
            if (note->version > 0) shape.version = note->version;
            shape.data = note->json;
            shape.createdBy = note->createdBy;
        }
        return shape;
    };
}

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

    if (lib_.metaValue("notesBackfill") == "1") {
        // A database from before notes were kept: its notes' versions are recorded but their
        // content never was. List every note once; the flag clears only when all are in.
        auto listing = api_.versions("items", 0, &err, "note");
        if (!listing) return void(rep.error = err);
        std::vector<std::string> missing;
        for (const auto& [key, version] : listing->versions) {
            auto have = lib_.zoteroNote(key);  // one a plugin is deleting too
            if (!have || have->version < version) missing.push_back(key);
        }
        if (!fetchAndApply("items", "notes", missing, rep, progress)) return;
        lib_.setMetaValue("notesBackfill", "0");
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
    pullPositions(rep);
}

// Reading positions: the settings lastPageIndex_u_<attachmentKey> changed since the last pull.
void SyncEngine::pullPositions(SyncReport& rep) {
    UserError err;
    auto listing = api_.settings(lib_.settingsVersion(), &err);
    if (!listing) {  // positions only: the rest of the sync stands; next time
        log(LogLevel::Warn, "sync: reading positions not fetched: %s", err.details.c_str());
        return;
    }
    static const std::string prefix = "lastPageIndex_u_";
    auto tx = lib_.begin();
    for (const auto& [key, setting] : listing->settings) {
        if (key.rfind(prefix, 0) != 0) continue;
        const auto& [value, version] = setting;
        if (value.is_number() || value.is_string()) lib_.applyRemotePosition(key.substr(prefix.size()), value.dump(), version);
    }
    if (listing->libraryVersion > 0) lib_.setSettingsVersion(listing->libraryVersion);
    tx.commit();
}

// The Kindle's reading positions that Zotero doesn't have yet, each against the setting version
// it was based on: one changed elsewhere meanwhile fails the batch (412), and the pull that
// follows turns it into a question for the reader instead of an overwrite.
bool SyncEngine::pushPositions(SyncReport& rep, bool& preconditionFailed) {
    preconditionFailed = false;
    std::vector<ReadingPosition> dirty = sendablePositions();
    for (size_t i = 0; i < dirty.size(); i += kBatch) {
        json body = json::object();
        const size_t end = std::min(dirty.size(), i + kBatch);
        {
            auto tx = lib_.begin();
            for (size_t j = i; j < end; ++j) {
                body["lastPageIndex_u_" + dirty[j].attachmentKey] = {{"value", json::parse(dirty[j].local)},
                                                                     {"version", dirty[j].remoteVersion}};
                lib_.markPositionSending(dirty[j].attachmentKey, dirty[j].local);  // its echo isn't news
            }
            tx.commit();
        }
        UserError err;
        auto res = api_.writeSettings(body, &err);
        if (!res) {
            rep.error = err;
            return false;
        }
        if (res->preconditionFailed) {
            preconditionFailed = true;
            return false;
        }
        auto tx = lib_.begin();
        for (size_t j = i; j < end; ++j) lib_.markPositionSent(dirty[j].attachmentKey, dirty[j].local, res->libraryVersion);
        tx.commit();
    }
    return true;
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
                if (local->deletedHere && local->createdHere && local->version == 0) {
                    // Our own upload, landed although its answer was lost, and deleted here since:
                    // keep the deletion, now against the server's version.
                    AnnotationRecord gone = *local;
                    gone.version = version;
                    lib_.upsertAnnotation(gone);
                    break;
                }
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
            lib_.upsertNote(parseNote(obj));
            break;
        case ObjectKind::Other:
            break;
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
    const bool known = lib_.objectVersion(key) > 0;
    if (lib_.zoteroNote(key)) lib_.removeObject("note", key);  // also one a plugin is deleting
    if (known) ++rep.itemsRemoved;  // notes count too; they share the key space
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
        // Read marks, on their own too: each item against its own version.
        if (!rep.error) pushReadMarks(rep);
        // Plugin changes, after the Kindle's own: each refused one on its own.
        if (!rep.error) pushPluginChanges(rep);
        // Read marks of items a plugin has just created in Zotero.
        if (!rep.error && rep.pluginSent > 0 && !lib_.readMarks().empty()) pushReadMarks(rep);
        // Reading positions, on their own: a refused one never holds annotations back.
        if (!rep.error && !pushPositions(rep, precondition) && precondition && !rep.error) {
            SyncReport pulled = pull(nullptr);  // the computer moved: the reader will ask
            if (pulled.error) rep.error = pulled.error;
            else if (!pushPositions(rep, precondition) && precondition)
                log(LogLevel::Warn, "sync: reading positions refused twice; next time");
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
    return n + static_cast<int>(sendablePositions().size() + lib_.readMarks().size() +
                                lib_.pendingPluginChanges().size());
}

// Each mark adds the read tag to its item or takes it off, sending the item's whole tag list
// (Zotero replaces it) against the item's version, so a tag changed on the desktop meanwhile is
// never lost: on a 412 the item is fetched again and the change made once more on Zotero's copy.
bool SyncEngine::pushReadMarks(SyncReport& rep) {
    for (const ReadMark& mark : lib_.readMarks()) {
        for (int attempt = 0;; ++attempt) {
            auto item = lib_.zoteroItem(mark.itemKey);  // a plugin's pending changes aren't sent here
            if (!item) {  // gone from Zotero: nothing to tag
                lib_.clearReadMark(mark);
                break;
            }
            if (item->version <= 0) break;  // a plugin's new item, not in Zotero yet: next time
            json data = json::parse(item->json, nullptr, false);
            if (!data.is_object()) data = json::object();
            json tags = data.contains("tags") && data["tags"].is_array() ? data["tags"] : json::array();
            json next = json::array();
            bool has = false;
            for (const auto& t : tags) {
                const bool same = t.is_object() && t.value("tag", "") == mark.tag;
                has = has || same;
                if (!same) next.push_back(t);
            }
            if (has == mark.read) {  // already so (marked on the desktop too)
                lib_.clearReadMark(mark);
                break;
            }
            if (mark.read) next.push_back({{"tag", mark.tag}});
            UserError err;
            auto res = api_.patchItem(mark.itemKey, {{"tags", next}}, item->version, &err);
            if (!res && err.kind == ErrorKind::Blocked) {  // this mark's own failure: the others still go
                log(LogLevel::Warn, "sync: read tag of %s blocked by the write guard; next time", mark.itemKey.c_str());
                break;
            }
            if (!res || !res->failed.empty()) {
                rep.error = err;
                return false;
            }
            if (res->gone) {  // deleted in Zotero meanwhile: the next pull removes it here too
                lib_.clearReadMark(mark);
                break;
            }
            if (res->preconditionFailed) {
                if (attempt > 0) {
                    log(LogLevel::Warn, "sync: read tag of %s refused twice; next time", mark.itemKey.c_str());
                    break;
                }
                UserError err2;
                auto objects = api_.fetch("items", {mark.itemKey}, &err2);
                if (!objects) {
                    rep.error = err2;
                    return false;
                }
                auto tx = lib_.begin();
                for (const auto& obj : *objects) apply(obj, rep);
                tx.commit();
                continue;
            }
            data["tags"] = next;
            item->json = data.dump();
            item->tags.clear();
            for (const auto& t : next) item->tags.push_back(t.value("tag", ""));
            item->version = res->libraryVersion;
            auto tx = lib_.begin();
            lib_.upsertItem(*item);
            lib_.setObjectVersion(mark.itemKey, res->libraryVersion);
            lib_.clearReadMark(mark);
            tx.commit();
            ++rep.itemsUpdated;
            break;
        }
    }
    return true;
}

// Plugin changes in dependency order. New items go first (≤50 per POST, against the library
// version), then new notes (one may go on an item made just before), then each edit on its own
// against its object's version. A change waiting on one not sent yet waits for the next sync.
bool SyncEngine::pushPluginChanges(SyncReport& rep) {
    using K = PluginChange::Kind;
    using S = PluginChange::State;
    const std::vector<PluginChange> initial = lib_.pendingPluginChanges();
    if (initial.empty()) return true;
    auto ready = [&](const PluginChange& c) {  // what it targets is in Zotero (or never will be)
        if (c.dependsOn.empty()) return true;
        auto d = lib_.pluginChange(c.dependsOn);
        return !d || d->state != S::Pending;
    };
    auto creates = [&](K kind) {
        std::vector<PluginChange> out;
        for (auto& c : lib_.pendingPluginChanges())
            if (c.kind == kind && ready(c)) out.push_back(std::move(c));
        return out;
    };
    bool ok = true;
    for (K kind : {K::CreateItem, K::AddNote}) {
        bool precondition = false;
        if (!pushPluginCreates(rep, creates(kind), precondition)) {
            ok = false;
            break;
        }
        if (!precondition) continue;
        // The library changed on the server since our version: pull, then once more.
        SyncReport pulled = pull(nullptr);
        rep.conflicts += pulled.conflicts;
        if (pulled.error) {
            rep.error = pulled.error;
            ok = false;
            break;
        }
        if (!pushPluginCreates(rep, creates(kind), precondition)) {
            ok = false;
            break;
        }
        if (precondition) log(LogLevel::Warn, "sync: plugin changes: Zotero kept changing (412 twice); next time");
    }
    if (ok)
        for (const auto& c : lib_.pendingPluginChanges()) {
            if (c.kind == K::CreateItem || c.kind == K::AddNote || !ready(c)) continue;
            auto now = lib_.pluginChange(c.id);  // an earlier one may have ended it (its item went)
            if (!now || now->state != S::Pending) continue;
            if (!pushPluginEdit(rep, *now)) {
                ok = false;
                break;
            }
        }
    for (const auto& c : initial) {
        auto now = lib_.pluginChange(c.id);
        if (now && now->state == S::Sent) ++rep.pluginSent;
        if (now && now->state == S::Refused) ++rep.pluginRefused;
    }
    log(LogLevel::Info, "sync: plugin changes: %d sent, %d refused, %d waiting", rep.pluginSent, rep.pluginRefused,
        static_cast<int>(lib_.pendingPluginChanges().size()));
    return ok;
}

// New items or notes, under the keys they have here (made here, like the Kindle's annotations: a
// lost answer can't make a second copy). False only if the connection failed.
bool SyncEngine::pushPluginCreates(SyncReport& rep, std::vector<PluginChange> changes, bool& preconditionFailed) {
    using K = PluginChange::Kind;
    preconditionFailed = false;
    std::vector<PluginChange> toSend;
    std::vector<json> objects;
    {
        auto tx = lib_.begin();
        for (const auto& c : changes) {
            if (lib_.objectVersion(c.itemKey) > 0) {  // in Zotero already: an earlier answer was lost
                lib_.markPluginChange(c.id, PluginChange::State::Sent, "", nowMs());
                continue;
            }
            std::string data;
            if (c.kind == K::CreateItem) {
                if (auto item = lib_.zoteroItem(c.itemKey)) data = item->json;
            } else if (auto note = lib_.zoteroNote(c.itemKey)) {
                data = note->json;
            }
            json obj = json::parse(data, nullptr, false);
            if (!obj.is_object()) {
                lib_.markPluginChange(c.id, PluginChange::State::Refused, "It is not in the library any more.");
                continue;
            }
            obj["key"] = c.itemKey;
            obj["version"] = 0;  // new: Zotero refuses it (412) if the key exists, never overwrites
            toSend.push_back(c);
            objects.push_back(std::move(obj));
        }
        tx.commit();
    }
    // One request per batch; if the guard refuses a batch, each of its objects is tried alone, so
    // a wrong one is refused without holding the others back.
    std::function<bool(size_t, size_t)> send = [&](size_t from, size_t to) -> bool {
        json batch = json::array();
        for (size_t j = from; j < to; ++j) batch.push_back(objects[j]);
        UserError err;
        auto res = api_.createItems(batch, lib_.libraryVersion(), &err);
        if (!res && err.kind == ErrorKind::Blocked) {
            if (to - from > 1) {
                for (size_t j = from; j < to; ++j)
                    if (!send(j, j + 1)) return false;
                return true;
            }
            lib_.markPluginChange(toSend[from].id, PluginChange::State::Refused, kNotAllowed);
            return true;
        }
        if (!res) {
            rep.error = err;
            return false;
        }
        if (res->preconditionFailed) {
            preconditionFailed = true;
            return true;
        }
        // Resolved by asking Zotero what it has under the key: an object refused as already there
        // (412: an earlier send landed), answered "unchanged", or a success without its copy.
        std::vector<size_t> lookUp;
        {
            auto tx = lib_.begin();
            std::set<size_t> answered;
            for (const auto& [index, key] : res->successKeys) {
                if (index < 0 || static_cast<size_t>(index) >= to - from) continue;
                answered.insert(index);
                const PluginChange& c = toSend[from + index];
                auto it = res->successful.find(index);
                if (it == res->successful.end() || it->second.value("key", "") != c.itemKey) {
                    lookUp.push_back(from + index);
                    continue;
                }
                apply(it->second, rep);  // Zotero's copy, at its version
                lib_.markPluginChange(c.id, PluginChange::State::Sent, "", nowMs());
            }
            lib_.setLibraryVersion(res->libraryVersion);  // our write was the only change since (else 412)
            for (const auto& [index, message] : res->failed) {
                if (index < 0 || static_cast<size_t>(index) >= to - from) continue;
                answered.insert(index);
                const PluginChange& c = toSend[from + index];
                auto code = res->failedCodes.find(index);
                if (code != res->failedCodes.end() && (code->second == 412 || code->second == 428)) {
                    lookUp.push_back(from + index);
                    continue;
                }
                lib_.markPluginChange(c.id, PluginChange::State::Refused, message);
            }
            for (size_t j = 0; j < to - from; ++j)
                if (!answered.count(j)) lookUp.push_back(from + j);  // "unchanged"
            tx.commit();
        }
        if (!lookUp.empty()) {
            std::vector<std::string> keys;
            for (size_t j : lookUp) keys.push_back(toSend[j].itemKey);
            UserError err2;
            auto there = api_.fetch("items", keys, &err2);
            if (!there) {
                rep.error = err2;
                return false;
            }
            auto tx = lib_.begin();
            for (const auto& obj : *there) {
                const std::string key = obj.value("key", "");
                for (size_t j : lookUp) {
                    if (toSend[j].itemKey != key) continue;
                    // Ours (sent before, its answer lost) unless it looks like something else.
                    const json& d = obj.contains("data") && obj["data"].is_object() ? obj["data"] : json::object();
                    const json& mine = objects[j];
                    const char* field = mine.value("itemType", "") == "note" ? "note" : "title";
                    if (d.value("itemType", "") != mine.value("itemType", "") || d.value(field, "") != mine.value(field, ""))
                        log(LogLevel::Warn, "sync: plugin change %s: what Zotero has under %s (%s) is not what was sent",
                            toSend[j].id.c_str(), key.c_str(), d.value("itemType", "").c_str());
                    apply(obj, rep);
                    lib_.markPluginChange(toSend[j].id, PluginChange::State::Sent, "", nowMs());
                }
            }
            tx.commit();  // one not there: left pending, tried again next time
        }
        return true;
    };
    for (size_t i = 0; i < objects.size() && !preconditionFailed; i += kBatch)
        if (!send(i, std::min(objects.size(), i + kBatch))) return false;
    return true;
}

// One tag, collection, trash or note deletion, against its object's version; made again on
// Zotero's fresh copy after a 412, once. False only if the connection failed.
bool SyncEngine::pushPluginEdit(SyncReport& rep, const PluginChange& c) {
    using K = PluginChange::Kind;
    using S = PluginChange::State;
    auto end = [&](S state, const std::string& message) {
        lib_.markPluginChange(c.id, state, message, state == S::Sent ? nowMs() : 0);
        return true;
    };
    for (int attempt = 0;; ++attempt) {
        std::optional<ItemRecord> item;
        json data, body;
        std::optional<WriteResult> res;
        UserError err;
        if (c.kind == K::DeleteNote) {
            auto note = lib_.zoteroNote(c.itemKey);
            if (!note) return end(S::Sent, "");  // gone already: what was asked
            // Only a note the same plugin added (the guard checks it too).
            if (note->createdBy.empty() || note->createdBy != c.pluginId)
                return end(S::Refused, "Only a note the plugin added can be deleted.");
            const int64_t version = note->version > 0 ? note->version : lib_.objectVersion(c.itemKey);
            if (version <= 0) return true;  // not in Zotero yet: next time
            res = api_.deleteItem(c.itemKey, version, &err);
        } else {
            item = lib_.zoteroItem(c.itemKey);  // Zotero's copy, without pending changes
            if (!item) return end(S::Refused, kDeletedInZotero);
            if (item->version <= 0) return true;  // not in Zotero yet: next time
            data = json::parse(item->json, nullptr, false);
            if (!data.is_object()) data = json::object();
            // The lists the guard checks against, should the data lack them.
            if (!data.contains("collections") || !data["collections"].is_array()) data["collections"] = item->collections;
            if (!data.contains("tags") || !data["tags"].is_array()) {
                data["tags"] = json::array();
                for (const auto& t : item->tags) data["tags"].push_back({{"tag", t}});
            }
            bool already = false;
            switch (c.kind) {
                case K::Trash:
                    already = deletedIn(data);
                    body = {{"deleted", 1}};
                    break;
                case K::AddTag:
                case K::RemoveTag: {
                    const bool add = c.kind == K::AddTag;
                    json next = json::array();
                    bool has = false;
                    if (data.contains("tags") && data["tags"].is_array())
                        for (const auto& t : data["tags"]) {
                            const bool same = t.is_object() && t.value("tag", "") == c.value;
                            has = has || same;
                            if (!same) next.push_back(t);
                        }
                    already = has == add;
                    if (add) next.push_back({{"tag", c.value}});
                    body = {{"tags", next}};
                    break;
                }
                default: {  // a collection
                    const bool add = c.kind == K::AddToCollection;
                    json next = json::array();
                    bool has = false;
                    if (data.contains("collections") && data["collections"].is_array())
                        for (const auto& k : data["collections"]) {
                            const bool same = k.is_string() && k.get<std::string>() == c.value;
                            has = has || same;
                            if (!same) next.push_back(k);
                        }
                    already = has == add;
                    if (add) next.push_back(c.value);
                    body = {{"collections", next}};
                    break;
                }
            }
            if (already) return end(S::Sent, "");  // so in Zotero already: nothing to send
            res = api_.patchItem(c.itemKey, body, item->version, &err);
        }
        if (!res && err.kind == ErrorKind::Blocked) return end(S::Refused, kNotAllowed);
        if (!res) {
            rep.error = err;
            return false;
        }
        // Gone from Zotero: a deletion asked for has happened; anything else can't be made.
        const bool removal = c.kind == K::DeleteNote || c.kind == K::Trash;
        if (res->gone) return removal ? end(S::Sent, "") : end(S::Refused, kDeletedInZotero);
        if (!res->failed.empty()) return end(S::Refused, res->failed.begin()->second);
        if (res->preconditionFailed) {
            if (attempt > 0) {
                log(LogLevel::Warn, "sync: plugin change %s on %s refused twice (412); next time", c.id.c_str(),
                    c.itemKey.c_str());
                lib_.markPluginChange(c.id, S::Pending, "Changed in Zotero meanwhile; it will be sent at the next sync.");
                return true;
            }
            UserError err2;
            auto objects = api_.fetch("items", {c.itemKey}, &err2);
            if (!objects) {
                rep.error = err2;
                return false;
            }
            if (objects->empty()) return removal ? end(S::Sent, "") : end(S::Refused, kDeletedInZotero);
            auto tx = lib_.begin();
            for (const auto& obj : *objects) {
                // Trashed in Zotero already: what the change asked for.
                if (c.kind == K::Trash && trashed(obj)) lib_.markPluginChange(c.id, S::Sent, "", nowMs());
                apply(obj, rep);
            }
            tx.commit();
            auto now = lib_.pluginChange(c.id);
            if (!now || now->state != S::Pending) return true;
            continue;
        }
        auto tx = lib_.begin();
        if (c.kind != K::DeleteNote && c.kind != K::Trash) {
            // Zotero's copy is the one sent now (its version: the library's after our write).
            for (const auto& [field, value] : body.items()) data[field] = value;
            item->json = data.dump();
            item->collections.clear();
            for (const auto& k : data["collections"])
                if (k.is_string()) item->collections.push_back(k.get<std::string>());
            item->tags.clear();
            for (const auto& t : data["tags"])
                if (t.is_object()) item->tags.push_back(t.value("tag", ""));
            item->version = res->libraryVersion;
            lib_.upsertItem(*item);
        }
        lib_.setObjectVersion(c.itemKey, res->libraryVersion);
        lib_.markPluginChange(c.id, S::Sent, "", nowMs());  // a trashed item or deleted note goes here too
        tx.commit();
        return true;
    }
}

// Reading positions to send: of the library's files, and well-formed.
std::vector<ReadingPosition> SyncEngine::sendablePositions() {
    std::vector<ReadingPosition> out;
    for (auto& p : lib_.dirtyPositions())
        if (lib_.attachment(p.attachmentKey) && !json::parse(p.local, nullptr, false).is_discarded())
            out.push_back(std::move(p));
    return out;
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
        std::vector<std::string> sent;
        for (const auto& a : pending)
            if (a.version == 0) sent.push_back(a.key);
        lib_.markUploadSent(sent);  // from here Zotero may have them, answer or not
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
