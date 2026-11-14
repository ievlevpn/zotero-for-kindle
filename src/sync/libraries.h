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
// or back after it had gone gets GET /groups/<id> (name, editing rules, owner, admins); the key's
// access (GET /keys/current) is read again when the list changed, or after kKeyCheckMs. A group no
// longer listed, or that the key can't read, is marked gone (its local copy stays, hidden, until
// the user removes it). False (with *err) if Zotero couldn't be asked: the groups aren't synced
// this time, My Library still is.
bool discoverGroups(ZoteroApi&, Library&, int64_t nowMs, UserError* err);

// The libraries a sync goes through, in order: My Library, then each enabled group still there.
std::vector<int64_t> librariesToSync(Library&);

// A sync's report over several libraries: counts added up, plugin events together, the first
// error kept; versions and `initial` are My Library's (the first report merged).
void mergeReport(SyncReport& into, SyncReport&& from);

}  // namespace zk
