#include "sync/libraries.h"

#include "core/log.h"

#include <algorithm>
#include <set>

namespace zk {

bool discoverGroups(ZoteroApi& api, Library& lib, int64_t nowMs, UserError* err) {
    UserError e;
    auto listing = api.groupVersions(&e);
    if (!listing) {
        log(LogLevel::Warn, "sync: groups not listed: %s (%s)", e.message.c_str(), e.details.c_str());
        if (err) *err = e;
        return false;
    }
    // What the Kindle has: every group it knows, gone or not.
    std::map<int64_t, LibraryInfo> known;
    for (auto& l : lib.libraries(true))
        if (l.id != kMyLibrary) known[l.id] = l;
    std::vector<int64_t> fetch;  // new, changed, or back
    for (const auto& [id, version] : *listing) {
        auto it = known.find(id);
        if (it == known.end() || it->second.gone || lib.groupMetaVersion(id) != version) fetch.push_back(id);
    }
    bool removed = false;
    for (const auto& [id, info] : known) removed = removed || (!info.gone && !listing->count(id));

    const std::string checked = lib.metaValue("zotero.keyCheckedMs");
    int64_t checkedMs = 0;
    try {
        checkedMs = checked.empty() ? 0 : std::stoll(checked);
    } catch (...) {
    }
    std::optional<KeyInfo> key;
    if (!fetch.empty() || removed || checked.empty() || nowMs - checkedMs >= kKeyCheckMs ||
        lib.keyGroupAccess() == GroupAccess::Unknown) {
        key = api.keyInfo(&e);
        if (!key) {
            log(LogLevel::Warn, "sync: key access not read: %s (%s)", e.message.c_str(), e.details.c_str());
            if (err) *err = e;
            return false;
        }
        lib.setUser(key->userId, !key->username.empty() ? key->username : key->displayName);
        lib.setKeyGroupAccess(key->allGroups && key->allGroups->read ? GroupAccess::All
                              : key->anyGroupAccess()               ? GroupAccess::Some
                                                                    : GroupAccess::None);
        lib.setMetaValue("zotero.keyCheckedMs", std::to_string(nowMs));
    }
    const int64_t me = key ? key->userId : lib.userId();
    for (int64_t id : fetch) {
        auto g = api.group(id, &e);
        if (!g) {
            // This group's metadata only: the others still sync. One that refuses us is out of reach.
            log(LogLevel::Warn, "sync: group %lld not read: %s (%s)", static_cast<long long>(id), e.message.c_str(),
                e.details.c_str());
            if (e.kind == ErrorKind::ApiKeyRejected || e.kind == ErrorKind::NotOnServer) {
                if (known.count(id)) lib.setGroupGone(id, true);
                continue;
            }
            if (err) *err = e;
            return false;
        }
        Library::GroupMeta meta;
        meta.id = id;
        meta.version = (*listing)[id];
        meta.name = g->name;
        meta.owner = g->owner;
        meta.libraryEditing = g->libraryEditing;
        meta.fileEditing = g->fileEditing;
        meta.admin = me != 0 && (g->owner == me || std::find(g->admins.begin(), g->admins.end(), me) != g->admins.end());
        lib.upsertGroup(meta);
        if (!known.count(id)) log(LogLevel::Info, "sync: group %lld (%s) found", static_cast<long long>(id), g->name.c_str());
    }
    for (const auto& [id, version] : *listing) {
        if (!lib.libraryInfo(id)) continue;  // its metadata failed above
        if (key) {
            const KeyInfo::Access a = key->groupAccess(id);
            lib.setGroupAccess(id, a.read, a.write);
            lib.setGroupGone(id, !a.read);
        }
    }
    for (const auto& [id, info] : known)
        if (!listing->count(id) && !info.gone) {
            log(LogLevel::Info, "sync: group %lld (%s) is no longer available", static_cast<long long>(id), info.name.c_str());
            lib.setGroupGone(id, true);
        }
    return true;
}

std::vector<int64_t> librariesToSync(Library& lib) {
    std::vector<int64_t> out{kMyLibrary};
    for (int64_t g : lib.syncedGroups()) out.push_back(g);
    return out;
}

void mergeReport(SyncReport& into, SyncReport&& from) {
    into.itemsAdded += from.itemsAdded;
    into.itemsUpdated += from.itemsUpdated;
    into.itemsRemoved += from.itemsRemoved;
    into.annotations += from.annotations;
    into.conflicts += from.conflicts;
    into.pluginSent += from.pluginSent;
    into.pluginRefused += from.pluginRefused;
    if (!into.error && from.error) into.error = from.error;
    into.pluginEvents.insert(into.pluginEvents.end(), std::make_move_iterator(from.pluginEvents.begin()),
                             std::make_move_iterator(from.pluginEvents.end()));
}

}  // namespace zk
