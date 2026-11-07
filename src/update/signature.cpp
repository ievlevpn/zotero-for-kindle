#include "update/signature.h"

extern "C" {
#include "monocypher-ed25519.h"
#include "monocypher.h"
}

#include <cstdio>
#include <vector>

namespace zk {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

void keyPair(const std::array<uint8_t, 32>& seed, uint8_t secret[64], uint8_t pub[32]) {
    uint8_t copy[32];  // Monocypher wipes the seed it is given
    std::copy(seed.begin(), seed.end(), copy);
    crypto_ed25519_key_pair(secret, pub, copy);
}

}  // namespace

std::string base64Encode(std::string_view in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = static_cast<uint8_t>(in[i]) << 16 |
                           (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? static_cast<uint8_t>(in[i + 2]) : 0);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += i + 1 < in.size() ? kAlphabet[n >> 6 & 63] : '=';
        out += i + 2 < in.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view in) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    for (char c : in) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') {
            ++pad;
            continue;
        }
        const int v = sextet(c);
        if (v < 0 || pad) return std::nullopt;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>(acc >> bits & 0xff);
        }
    }
    if (pad > 2) return std::nullopt;
    return out;
}

bool verifySignature(std::string_view message, std::string_view sigBase64, const std::array<uint8_t, 32>& publicKey) {
    auto sig = base64Decode(sigBase64);
    if (!sig || sig->size() != 64) return false;
    return crypto_ed25519_check(reinterpret_cast<const uint8_t*>(sig->data()), publicKey.data(),
                                reinterpret_cast<const uint8_t*>(message.data()), message.size()) == 0;
}

std::string sign(std::string_view message, const std::array<uint8_t, 32>& seed) {
    uint8_t secret[64], pub[32], sig[64];
    keyPair(seed, secret, pub);
    crypto_ed25519_sign(sig, secret, reinterpret_cast<const uint8_t*>(message.data()), message.size());
    crypto_wipe(secret, sizeof secret);
    return base64Encode(std::string_view(reinterpret_cast<const char*>(sig), sizeof sig));
}

std::array<uint8_t, 32> publicKeyFor(const std::array<uint8_t, 32>& seed) {
    uint8_t secret[64];
    std::array<uint8_t, 32> pub{};
    keyPair(seed, secret, pub.data());
    crypto_wipe(secret, sizeof secret);
    return pub;
}

std::optional<std::string> sha512FileHex(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::nullopt;
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    std::vector<uint8_t> buf(64 * 1024);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) crypto_sha512_update(&ctx, buf.data(), n);
    const bool failed = std::ferror(f);
    std::fclose(f);
    if (failed) return std::nullopt;
    uint8_t hash[64];
    crypto_sha512_final(&ctx, hash);
    std::string hex;
    char two[3];
    for (uint8_t b : hash) {
        std::snprintf(two, sizeof two, "%02x", b);
        hex += two;
    }
    return hex;
}

}  // namespace zk
