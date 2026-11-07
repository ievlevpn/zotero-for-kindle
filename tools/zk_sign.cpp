// zk-sign: the release signing key and signatures (Ed25519, as the app verifies them).
//   zk-sign keygen SEED_FILE      a new key: the seed (base64, mode 0600) into SEED_FILE; prints
//                                 the public key as a C++ initializer for src/update/release_key.h
//   zk-sign sign SEED_FILE FILE   prints FILE's signature (base64), for update.json.sig
//   zk-sign verify FILE SIG_FILE  checks a signature against the built-in public key

#include "update/release_key.h"
#include "update/signature.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace zk;

namespace {

std::optional<std::string> slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<std::array<uint8_t, 32>> readSeed(const char* path) {
    auto text = slurp(path);
    auto raw = text ? base64Decode(*text) : std::nullopt;
    if (!raw || raw->size() != 32) return std::nullopt;
    std::array<uint8_t, 32> seed;
    std::memcpy(seed.data(), raw->data(), 32);
    return seed;
}

int usage() {
    std::fprintf(stderr, "usage: zk-sign keygen SEED_FILE | sign SEED_FILE FILE | verify FILE SIG_FILE\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "keygen")) {
        std::array<uint8_t, 32> seed;
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, seed.data(), seed.size()) != 32) return 1;
        close(fd);
        fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            std::perror(argv[2]);
            return 1;
        }
        const std::string text = base64Encode(std::string_view(reinterpret_cast<char*>(seed.data()), 32)) + "\n";
        if (write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size())) return 1;
        close(fd);
        const auto pub = publicKeyFor(seed);
        std::printf("{");
        for (size_t i = 0; i < pub.size(); ++i) std::printf("%s0x%02x", i ? ", " : "", pub[i]);
        std::printf("}\n");
        return 0;
    }
    if (argc == 4 && !std::strcmp(argv[1], "sign")) {
        auto seed = readSeed(argv[2]);
        auto file = slurp(argv[3]);
        if (!seed || !file) return usage();
        std::printf("%s\n", sign(*file, *seed).c_str());
        return 0;
    }
    if (argc == 4 && !std::strcmp(argv[1], "verify")) {
        auto file = slurp(argv[2]);
        auto sig = slurp(argv[3]);
        if (!file || !sig) return usage();
        const bool ok = verifySignature(*file, *sig, kReleasePublicKey);
        std::printf("%s\n", ok ? "ok" : "BAD SIGNATURE");
        return ok ? 0 : 1;
    }
    return usage();
}
