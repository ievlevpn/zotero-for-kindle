#pragma once
// Downloading a release, verified before it is kept: the manifest's signature (the built-in
// key), its version (the release's own, so an old signed manifest can't be replayed), then the
// zip's size and SHA-512. Anything that fails leaves nothing in workDir.

#include "net/errors.h"
#include "net/http.h"
#include "update/release_check.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace zk {

struct DownloadHooks {
    std::function<int64_t(const std::string& dir)> freeSpace;  // bytes; default statvfs
    std::function<NetState()> probe;                          // default probeNetState
    int64_t extraBytes = 0;  // also needed while installing: the library backup
};

// The zip is written as workDir/zotero-for-kindle.zip (*zipPath). Needs zipSize * 4 + extraBytes
// + 64 MB free: the zip, its unpacked tree, the app's backup and the library's (extraBytes).
bool downloadRelease(Http&, const ReleaseInfo&, const std::array<uint8_t, 32>& publicKey, const std::string& workDir,
                     std::function<void(int64_t done, int64_t total)> progress,
                     std::shared_ptr<std::atomic<bool>> cancel, std::string* zipPath, UserError*,
                     DownloadHooks hooks = {});

}  // namespace zk
