#pragma once
// Release signatures: Ed25519 (SHA-512) over the manifest's exact bytes, carried as base64.
// Monocypher does the cryptography; zk-sign uses the same functions to sign.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zk {

std::string base64Encode(std::string_view);
std::optional<std::string> base64Decode(std::string_view);  // whitespace ignored; nullopt if malformed

bool verifySignature(std::string_view message, std::string_view sigBase64, const std::array<uint8_t, 32>& publicKey);
std::string sign(std::string_view message, const std::array<uint8_t, 32>& seed);  // base64 signature
std::array<uint8_t, 32> publicKeyFor(const std::array<uint8_t, 32>& seed);

std::optional<std::string> sha512FileHex(const std::string& path);  // lowercase hex; nullopt if unreadable

}  // namespace zk
