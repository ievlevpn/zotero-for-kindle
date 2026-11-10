#pragma once
// The plugin catalog's registry: registry.json, signed with the app's release key, listing each
// plugin with its author's public key, and the withdrawn ("revoked") ids.

#include "update/version.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zk::catalog {

struct Entry {
    std::string id, name, author, description, repo;  // repo: "owner/name" on GitHub
    std::array<uint8_t, 32> publicKey{};               // the author's Ed25519 key for plugin.zip.sig
};
struct Revoked { std::string id, reason; };
struct Registry {
    std::string updated;
    std::vector<Entry> plugins;
    std::vector<Revoked> revoked;
};

constexpr size_t kMaxRegistryBytes = 1 << 20;  // 1 MB

// The whole file or nothing: any bad part (wrong type, duplicate or malformed id, a key that
// isn't 32 bytes, a repo that isn't owner/name, over 1 MB) rejects it. Unknown fields are ignored.
std::optional<Registry> parseRegistry(std::string_view json, std::string* why);

// A release tag as a version: "v1.2.0" or "1.2.0".
std::optional<Version> versionFromTag(std::string_view tag);

}  // namespace zk::catalog
