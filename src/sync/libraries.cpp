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
    // What the Kindle has: every group it knows, gone, unreadable or not.
    std::map<int64_t, LibraryInfo> known;
    for (auto& l : lib.libraries(true))
        if (l.id != kMyLibrary) known[l.id] = l;
    bool listChanged = false;
    for (const auto& [id, version] : *listing) {
        auto it = known.find(id);
        listChanged = listChanged || it == known.end() || it->second.gone || lib.groupMetaVersion(id) != version;
    }
    for (const auto& [id, info] : known) listChanged = listChanged || (!info.gone && !listing->count(id));

    // The key's access: read again when the list changed, the key or account changed, or a day on
    // (a group's pass that Zotero refused clears zotero.keyCheckedMs). Never for an account with no
    // groups and none known: nothing to read it for (0.7.0 made no such request either).
    const std::string fingerprint = api.keyFingerprint();
    const bool keyChanged = lib.metaValue("zotero.keyFingerprint") != fingerprint;
    const std::string checked = lib.metaValue("zotero.keyCheckedMs");
    int64_t checkedMs = 0;
    try {
        checkedMs = checked.empty() ? 0 : std::stoll(checked);
    } catch (...) {
    }
    const bool anyGroups = !listing->empty() || !known.empty();
    std::optional<KeyInfo> key;
    if (anyGroups && (listChanged || keyChanged || checked.empty() || nowMs - checkedMs >= kKeyCheckMs ||
                      lib.keyGroupAccess() == GroupAccess::Unknown)) {
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
        lib.setMetaValue("zotero.keyFingerprint", fingerprint);
    } else if (keyChanged && !anyGroups) {
        // A new key on an account with no groups: what the old key could read no longer says anything.
        lib.setKeyGroupAccess(GroupAccess::Unknown);
        lib.setMetaValue("zotero.keyCheckedMs", "");
        lib.setMetaValue("zotero.keyFingerprint", fingerprint);
    }

    // Metadata (name, editing rules): a group new to the Kindle, changed, back after it had gone, or
    // readable now (key re-read) without its rules ever read. An unreadable one is not asked about
    // again until the listing or the key changes.
    std::vector<int64_t> fetch;
    for (const auto& [id, version] : *listing) {
        auto it = known.find(id);
        const bool readable = key ? key->groupAccess(id).read : it != known.end() && !it->second.unreadable;
        if (it == known.end() || it->second.gone || lib.groupMetaVersion(id) != version ||
            (readable && !lib.groupMetaKnown(id)))
            fetch.push_back(id);
    }
    const int64_t me = key ? key->userId : lib.userId();
    std::set<int64_t> refused;  // GET /groups/<id> refused: the key can't read it
    for (int64_t id : fetch) {
        auto g = api.group(id, &e);
        if (!g) {
            log(LogLevel::Warn, "sync: group %lld not read: %s (%s)", static_cast<long long>(id), e.message.c_str(),
                e.details.c_str());
            if (e.kind == ErrorKind::ApiKeyRejected || e.kind == ErrorKind::NotOnServer || e.details == "HTTP 404") {
                // Listed but out of the key's reach: kept as such (not gone), and not asked again
                // at this listing version.
                auto it = known.find(id);
                lib.markGroupUnreadable(id, (*listing)[id], it == known.end() ? std::string{} : it->second.name);
                refused.insert(id);
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
        meta.libraryEditing = g->libraryEditing.empty() ? "admins" : g->libraryEditing;
        meta.fileEditing = g->fileEditing.empty() ? "admins" : g->fileEditing;
        meta.admin = me != 0 && (g->owner == me || std::find(g->admins.begin(), g->admins.end(), me) != g->admins.end());
        lib.upsertGroup(meta);
        if (!known.count(id)) log(LogLevel::Info, "sync: group %lld (%s) found", static_cast<long long>(id), g->name.c_str());
    }
    if (key)
        for (const auto& [id, version] : *listing) {
            if (refused.count(id) || !lib.libraryInfo(id)) continue;
            const KeyInfo::Access a = key->groupAccess(id);
            lib.setGroupAccess(id, a.read, a.write);  // a.read false: listed, unreadable (not gone)
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
    into.initial = into.initial || from.initial;  // any library's first full sync: plugins rebuild
    into.groupErrors.insert(into.groupErrors.end(), from.groupErrors.begin(), from.groupErrors.end());
    into.pluginEvents.insert(into.pluginEvents.end(), std::make_move_iterator(from.pluginEvents.begin()),
                             std::make_move_iterator(from.pluginEvents.end()));
}

std::string groupErrorMessage(const UserError& e) {
    if (e.kind == ErrorKind::ApiKeyRejected || e.kind == ErrorKind::ApiKeyNoWrite)
        return "Your API key can't sync this group.";
    return e.message;
}

void scopeGroupError(SyncReport& rep, int64_t group, Library& lib) {
    if (!rep.error || group == kMyLibrary) return;
    SyncReport::GroupError g;
    g.lib = group;
    g.name = lib.libraryName(group);
    g.error = *rep.error;
    g.message = groupErrorMessage(*rep.error);
    log(LogLevel::Warn, "sync: group %lld (%s) can't sync: %s (%s)", static_cast<long long>(group), g.name.c_str(),
        rep.error->message.c_str(), rep.error->details.c_str());
    if (rep.error->kind == ErrorKind::ApiKeyRejected || rep.error->kind == ErrorKind::ApiKeyNoWrite)
        lib.setMetaValue("zotero.keyCheckedMs", "");  // the key's access may have changed: read it next time
    rep.error.reset();
    rep.groupErrors.push_back(std::move(g));
}

void recordGroupErrors(Library& lib, const std::vector<int64_t>& groups, const SyncReport& rep) {
    for (int64_t g : groups) {
        std::string message;
        for (const auto& e : rep.groupErrors)
            if (e.lib == g) {
                message = e.message;
                break;
            }
        lib.setGroupSyncError(g, message);
    }
}

bool unreachable(const UserError& e) {
    switch (e.kind) {
        case ErrorKind::NoWifi:
        case ErrorKind::TailscaleDown:
        case ErrorKind::HostNotFound:
        case ErrorKind::HostNotAnswering:
        case ErrorKind::Certificate: return true;
        default: return e.kind == ErrorKind::Other && e.details.rfind("curl ", 0) == 0;
    }
}

}  // namespace zk
