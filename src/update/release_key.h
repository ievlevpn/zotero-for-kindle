#pragma once
// The public half of the release signing key (tools/release.sh signs with the private half,
// kept off the repository in local/release-key). Updates are installed only if their manifest
// verifies with this key.

#include <array>
#include <cstdint>

namespace zk {

inline constexpr std::array<uint8_t, 32> kReleasePublicKey{};  // set in Task 7 (zk-sign keygen)

}  // namespace zk
