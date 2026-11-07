#include "update/version.h"

#include <charconv>

namespace zk {

std::string Version::str() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::optional<Version> parseVersion(std::string_view s) {
    if (!s.empty() && s.front() == 'v') s.remove_prefix(1);
    int parts[3];
    for (int i = 0; i < 3; ++i) {
        const size_t end = i < 2 ? s.find('.') : s.size();
        if (end == std::string_view::npos || end == 0) return std::nullopt;
        const std::string_view digits = s.substr(0, end);
        auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), parts[i]);
        if (ec != std::errc{} || p != digits.data() + digits.size()) return std::nullopt;
        s.remove_prefix(i < 2 ? end + 1 : end);
    }
    return Version{parts[0], parts[1], parts[2]};
}

Version appVersion() {
    static const Version v = parseVersion(ZK_VERSION).value_or(Version{});
    return v;
}

}  // namespace zk
