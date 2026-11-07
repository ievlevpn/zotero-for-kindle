#include "update/manifest.h"

#include <nlohmann/json.hpp>

namespace zk {

std::optional<Manifest> parseManifest(const std::string& text) {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("version") || !j["version"].is_string() || !j.contains("zip") ||
        !j["zip"].is_object())
        return std::nullopt;
    const auto& zip = j["zip"];
    if (!zip.contains("name") || !zip["name"].is_string() || !zip.contains("size") ||
        !zip["size"].is_number_integer() || !zip.contains("sha512") || !zip["sha512"].is_string())
        return std::nullopt;
    auto version = parseVersion(j["version"].get<std::string>());
    Manifest m;
    m.zipName = zip["name"].get<std::string>();
    m.size = zip["size"].get<int64_t>();
    m.sha512 = zip["sha512"].get<std::string>();
    if (!version || m.zipName.empty() || m.size <= 0 || m.sha512.size() != 128 ||
        m.sha512.find_first_not_of("0123456789abcdef") != std::string::npos)
        return std::nullopt;
    m.version = *version;
    return m;
}

}  // namespace zk
