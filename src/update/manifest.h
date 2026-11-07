#pragma once
// update.json, the signed part of a release: which version it is and exactly which zip.
//   {"version":"0.3.0","zip":{"name":"zotero-for-kindle.zip","size":15234567,"sha512":"<hex>"}}

#include "update/version.h"

#include <cstdint>
#include <optional>
#include <string>

namespace zk {

struct Manifest {
    Version version;
    std::string zipName;
    int64_t size = 0;
    std::string sha512;  // 128 lowercase hex digits
};

std::optional<Manifest> parseManifest(const std::string& json);

}  // namespace zk
