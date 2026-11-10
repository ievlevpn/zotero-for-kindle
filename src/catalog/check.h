#pragma once
// Checking the catalog: the registry (verified with the release key) and, for each installed
// plugin in it, its repo's latest release on GitHub. What was found is kept in
// <pluginsDir>/.catalog/ so the catalog shows offline:
//   registry.json, registry.json.sig  the registry as fetched (written by runCheck only)
//   latest.json                       {"registryMs", "plugins": {id: {version, zipUrl, sigUrl, zipSize, checkedMs}}}
//   installed.json                    {id: "catalog"} for plugins installed from the catalog

#include "catalog/registry.h"
#include "net/http.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace zk::catalog {

extern const char* const kRegistryFeed;  // api.github.com/repos/ievlevpn/zotero-for-kindle-plugins/releases/latest
constexpr int64_t kPluginCheckEveryMs = 6LL * 3600 * 1000;

struct Latest {
    Version version;
    std::string zipUrl, sigUrl;
    int64_t zipSize = 0;
    int64_t checkedMs = 0;
};

struct Cache {
    std::optional<Registry> registry;
    int64_t registryMs = 0;  // when the registry was last fetched and verified (wall ms)
    std::map<std::string, Latest> latest;
    std::set<std::string> fromCatalog;  // installed from the catalog (else: by hand)
};

std::string cacheDir(const std::string& pluginsDir);  // <pluginsDir>/.catalog

Cache loadCache(const std::string& pluginsDir);  // missing/bad files → empty parts
// Writes latest.json and installed.json (tmp + rename each). The registry files are written by
// runCheck from the verified bytes, so their signature stays checkable.
bool saveCache(const std::string& pluginsDir, const Cache&);

const Entry* find(const Cache&, std::string_view id);
// The latest release found for `id`, if it is newer than installedVersion.
std::optional<Latest> updateFor(const Cache&, std::string_view id, std::string_view installedVersion);

struct CheckOptions {
    std::string feed;  // the registry's latest-release URL (kRegistryFeed, or ZK_PLUGIN_REGISTRY)
    std::array<uint8_t, 32> registryKey;
    std::function<int64_t()> wallMs;
};
enum class CheckStatus { Ok, Offline, Failed };
struct CheckResult {
    CheckStatus status;
    std::string message;
};

// One plugin's latest release (one API call): nullopt when it can't be read or lacks plugin.zip /
// plugin.zip.sig. checkedMs = nowMs.
std::optional<Latest> fetchLatest(Http&, const Entry&, int64_t nowMs, std::shared_ptr<std::atomic<bool>> cancel);

// Fetches the registry (verified with registryKey, kept only if it verifies and parses), then the
// latest release of each installed id that is in the registry and wasn't checked within 6 h.
// A plugin release that can't be read or lacks plugin.zip / plugin.zip.sig is skipped quietly.
CheckResult runCheck(Http&, const CheckOptions&, const std::string& pluginsDir,
                     const std::vector<std::string>& installedIds, bool force,
                     std::shared_ptr<std::atomic<bool>> cancel);

}  // namespace zk::catalog
