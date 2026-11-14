#pragma once
// Group libraries in the sync (spec: group libraries): finding the user's groups, and the order a
// sync goes through the libraries.

#include "net/errors.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

#include <cstdint>
#include <vector>

namespace zk {

// How long the key's access (GET /keys/current) is trusted when the group list hasn't changed.
constexpr int64_t kKeyCheckMs = int64_t{24} * 3600 * 1000;

// One request per sync: GET /users/<id>/groups?format=versions. A group new to the Kindle, changed,
// back after it had gone, or newly readable without its rules read gets GET /groups/<id> (name,
// editing rules, owner, admins). The key's access (GET /keys/current) is read again when the list
// changed, the key or account changed (a fingerprint in meta), a group's pass was refused, or after
// kKeyCheckMs — and never while there are no groups at all. A group no longer listed is marked
// gone; one listed that the key can't read is kept as unreadable (not gone, not asked about again
// until its listing or the key changes). Nothing is deleted. False (with *err) if Zotero couldn't be
// asked: the groups aren't synced this time, My Library still is (unless Zotero is unreachable:
// see unreachable()).
bool discoverGroups(ZoteroApi&, Library&, int64_t nowMs, UserError* err);

// The libraries a sync goes through, in order: My Library, then each enabled group still there.
std::vector<int64_t> librariesToSync(Library&);

// A sync's report over several libraries: counts added up, plugin events and group errors
// together, the first error kept; versions are My Library's (the first report merged), `initial`
// is set when any library's pull was its first full sync (plugins rebuild what they keep).
void mergeReport(SyncReport& into, SyncReport&& from);

// A group's pass (push or pull) that failed: its error becomes the report's groupErrors entry, so it
// reads as the group's ("Lab: can't sync"), not the account's, and doesn't stop the rest. Zotero
// refusing access (401/403) also has the key's access read again at the next discovery.
void scopeGroupError(SyncReport&, int64_t group, Library&);
// After a sync's passes: each group gone through keeps its error for Settings, or has it cleared.
void recordGroupErrors(Library&, const std::vector<int64_t>& groups, const SyncReport&);
// The user's words for a group's failure.
std::string groupErrorMessage(const UserError&);

// Zotero couldn't be reached at all (no Wi-Fi, no DNS, no answer): the rest of the sync would only
// fail the same way.
bool unreachable(const UserError&);

}  // namespace zk
