#pragma once
// Release versions: X.Y.Z, optionally written with a leading "v" (git tags).

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace zk {

struct Version {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
    std::string str() const;
};

std::optional<Version> parseVersion(std::string_view);
Version appVersion();  // this build's (ZK_VERSION)

}  // namespace zk
