#pragma once
// Installing a plugin from the catalog: download, verify, stage, swap, roll back, remove, repair.
//
// Folders under pluginsDir:
//   <id>/            the installed plugin; its data/ stays here across update and roll back
//   .previous/<id>/  the one kept previous version (code only, never data/)
//   .incoming/       staging; <id>.zip while downloading, <id>/ once verified; cleared at start
//
// Every swap is a sequence of renames ordered so that a kill at any point leaves a state
// repairInterrupted turns into one working plugin with its data/:
//   1. .previous/<id> → .incoming/ (out of the way; deleted later)
//   2. <id> → .previous/<id>            killed here: <id> missing → .previous/<id> is put back
//   3. the new code → <id>              killed here: data/ still in .previous/<id> → moved to <id>
//   4. .previous/<id>/data → <id>/data
// A zip's own data/ (any case, and "data." / "data " as FAT folds them) is never unpacked, and a
// zip entry with a "." or empty path part is refused, so the user's data/ is never overwritten.
// No data/ folder is ever deleted outside removeFiles: commit and rollBack first move a data/ left
// in .previous/<id> back to <id>, and refuse (changing nothing) if <id> has a data/ of its own.

#include "catalog/check.h"
#include "catalog/registry.h"
#include "net/errors.h"
#include "net/http.h"
#include "plugins/manifest.h"
#include "update/download.h"
#include "update/version.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zk::catalog {

constexpr int64_t kMaxZipBytes = 8LL << 20;    // plugin.zip, claimed and received
constexpr int64_t kMaxTreeBytes = 32LL << 20;  // the unpacked tree
constexpr unsigned kMaxZipEntries = 2000;

struct Staged {
    PluginManifest manifest;
    std::vector<Permission> added;  // vs the installed version (all if new); Net if it adds a host
};

// Downloads plugin.zip + .sig to .incoming/, verifies (key, size, space), unpacks to
// .incoming/<id>/ (no escapes/symlinks, ≤ 32 MB), checks id/version/api/minApp. On failure nothing
// is left in .incoming/ for this id. `hooks` (freeSpace, probe) are for tests.
std::optional<Staged> stage(Http&, const Entry&, const Latest&, const std::string& pluginsDir, Version app,
                            std::shared_ptr<std::atomic<bool>> cancel, UserError*, DownloadHooks hooks = {});

// <id>/ → .previous/<id>/ (replacing an older kept one), .incoming/<id>/ → <id>/, moving the old
// data/ across; marks provenance "catalog". The caller has stopped the plugin.
bool commit(const std::string& pluginsDir, const std::string& id, UserError*);
// Swaps <id> and .previous/<id>; data/ stays in <id>. The caller has stopped the plugin.
bool rollBack(const std::string& pluginsDir, const std::string& id, UserError*);
// The version in .previous/<id>/plugin.json, if one is kept and readable.
std::optional<std::string> previousVersion(const std::string& pluginsDir, const std::string& id);
// <id>/ (with its data/), .previous/<id>/, .incoming/<id>/ and its provenance; nothing else.
bool removeFiles(const std::string& pluginsDir, const std::string& id, UserError*);
// At start, before the scan: <id> missing + .previous/<id> → put back; data/ left in
// .previous/<id> → back to <id>; .incoming/ cleared.
void repairInterrupted(const std::string& pluginsDir);
// The same for one id, leaving .incoming/ alone (other ids' staged updates may be waiting there).
// Call it after a failed commit or rollBack, before reloading the plugin.
void repairOne(const std::string& pluginsDir, const std::string& id);

}  // namespace zk::catalog
