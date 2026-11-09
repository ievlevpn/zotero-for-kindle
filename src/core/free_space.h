#pragma once
// Free bytes on the partition holding `path` (for an unprivileged user). Unknown (statvfs failed):
// UINT64_MAX, so callers never block on a probe that can't answer.

#include <sys/statvfs.h>

#include <cstdint>
#include <string>

namespace zk {

inline uint64_t freeBytesAt(const std::string& path) {
    struct statvfs st {};
    if (::statvfs(path.c_str(), &st) != 0) return UINT64_MAX;
    return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
}

}  // namespace zk
