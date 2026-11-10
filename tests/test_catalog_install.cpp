#include <doctest/doctest.h>

#include "catalog/install.h"
#include "fn_http.h"
#include "update/signature.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace zk;
using namespace zk::catalog;
using zk::test::FnHttp;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const std::array<uint8_t, 32> kSeed = {4, 5, 6};
const std::array<uint8_t, 32> kOtherSeed = {7, 8, 9};
const std::string kBase = "https://github.com/a/zk-feed-riffle/releases/download/v1.2.0/";
const std::string kZipUrl = kBase + "plugin.zip";
const std::string kSigUrl = kBase + "plugin.zip.sig";
const Version kApp{1, 0, 0};

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("zk-catinst-" + std::to_string(::getpid()) + "-" + std::to_string(++n) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string manifest(const std::string& version = "1.2.0", const json& perms = json::array({"library:read"}),
                     const std::string& id = "feed-riffle", const std::string& minApp = "0.1.0") {
    return json{{"id", id}, {"name", "Feed Riffle"}, {"version", version}, {"api", 0}, {"main", "main.js"},
                {"minApp", minApp}, {"permissions", perms}}
        .dump();
}

// A zip in memory. Entries named in `symlinks` are marked as Unix symlinks.
std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files,
                    const std::set<std::string>& symlinks = {}) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, text] : files)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), text.data(), text.size(), MZ_BEST_COMPRESSION));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
    std::string out(static_cast<char*>(buf), size);
    mz_zip_writer_end(&zip);
    for (const auto& link : symlinks) {  // the central directory's external attributes: S_IFLNK | 0777
        for (size_t p = 0; (p = out.find("PK\x01\x02", p)) != std::string::npos; p += 4) {
            const size_t nameLen = static_cast<uint8_t>(out[p + 28]) | (static_cast<uint8_t>(out[p + 29]) << 8);
            if (out.compare(p + 46, nameLen, link) != 0) continue;
            const uint32_t attr = 0120777u << 16;
            std::memcpy(&out[p + 38], &attr, 4);  // little-endian host
            out[p + 5] = 3;                       // made by Unix
        }
    }
    return out;
}

std::string goodZip(const std::string& version = "1.2.0", const json& perms = json::array({"library:read"})) {
    return makeZip({{"plugin.json", manifest(version, perms)}, {"main.js", "// v" + version}});
}

Entry entry(const std::array<uint8_t, 32>& seed = kSeed) {
    Entry e;
    e.id = "feed-riffle";
    e.name = "Feed Riffle";
    e.repo = "a/zk-feed-riffle";
    e.publicKey = publicKeyFor(seed);
    return e;
}

Latest latest(const std::string& zip, Version v = {1, 2, 0}) {
    return {v, kZipUrl, kSigUrl, static_cast<int64_t>(zip.size()), 0};
}

// Serves the zip and its signature; records what was asked for.
struct Server {
    std::map<std::string, std::string> files;
    std::vector<std::string> sent;
    FnHttp http{[this](const HttpRequest& r) {
        sent.push_back(r.url);
        HttpResponse resp;
        auto it = files.find(r.url);
        if (it == files.end()) {
            resp.status = 404;
            return resp;
        }
        resp.status = 200;
        if (!r.outFile.empty()) std::ofstream(r.outFile, std::ios::binary) << it->second;
        else resp.body = it->second;
        return resp;
    }};
    Server(const std::string& zip, const std::array<uint8_t, 32>& seed = kSeed) {
        files[kZipUrl] = zip;
        files[kSigUrl] = sign(zip, seed);
    }
};

DownloadHooks roomy() {
    DownloadHooks h;
    h.freeSpace = [](const std::string&) { return int64_t{1} << 40; };
    h.probe = [] { return NetState{}; };
    return h;
}

bool incomingEmpty(const TempDir& t) {
    std::error_code ec;
    const fs::path in = t.path / ".incoming";
    return !fs::exists(in, ec) || fs::is_empty(in, ec);
}

// An installed 1.0.0 with the user's data.
void installOld(const TempDir& t) {
    writeFile(t.path / "feed-riffle/plugin.json", manifest("1.0.0"));
    writeFile(t.path / "feed-riffle/main.js", "// v1.0.0");
    writeFile(t.path / "feed-riffle/data/notes.txt", "mine");
}

std::optional<Staged> stageZip(TempDir& t, const std::string& zip, UserError* err,
                               const std::array<uint8_t, 32>& signer = kSeed, Latest l = {}) {
    Server s(zip, signer);
    if (l.zipUrl.empty()) l = latest(zip);
    return stage(s.http, entry(), l, t.str(), kApp, nullptr, err, roomy());
}

void checkRefused(TempDir& t, const std::string& zip, const std::string& message, Latest l = {},
                  const std::array<uint8_t, 32>& signer = kSeed) {
    installOld(t);
    UserError err;
    CHECK_FALSE(stageZip(t, zip, &err, signer, l));
    CHECK(err.message == message);
    CHECK(incomingEmpty(t));
    CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.0.0");
    CHECK(readFile(t.path / "feed-riffle/data/notes.txt") == "mine");
}

}  // namespace

TEST_CASE("catalog install: a good install stages, commits and marks the provenance") {
    TempDir t;
    UserError err;
    auto staged = stageZip(t, goodZip(), &err);
    REQUIRE_MESSAGE(staged, err.details);
    CHECK(staged->manifest.id == "feed-riffle");
    CHECK(staged->manifest.version == "1.2.0");
    CHECK(staged->added == std::vector<Permission>{Permission::LibraryRead});  // new: all of them
    CHECK(fs::exists(t.path / ".incoming/feed-riffle/plugin.json"));
    CHECK_FALSE(fs::exists(t.path / "feed-riffle"));  // not installed until commit

    REQUIRE(commit(t.str(), "feed-riffle", &err));
    CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.2.0");
    CHECK(incomingEmpty(t));
    CHECK(loadCache(t.str()).fromCatalog.count("feed-riffle"));
    CHECK_FALSE(previousVersion(t.str(), "feed-riffle"));
    const auto scan = scanPlugins(t.str(), kApp);
    REQUIRE(scan.size() == 1);  // .incoming, .previous and .catalog are not plugins
    CHECK(scan[0].problem.empty());
}

TEST_CASE("catalog install: each refusal leaves .incoming empty and the installed plugin as it was") {
    TempDir t;
    SUBCASE("bad signature") {
        const std::string zip = goodZip();
        installOld(t);
        Server s(zip);
        s.files[kSigUrl] = sign(zip + "x", kSeed);
        UserError err;
        CHECK_FALSE(stage(s.http, entry(), latest(zip), t.str(), kApp, nullptr, &err, roomy()));
        CHECK(err.message == "The download isn't signed by the plugin's author.");
        CHECK(incomingEmpty(t));
        CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.0.0");
    }
    SUBCASE("wrong key") {
        checkRefused(t, goodZip(), "The download isn't signed by the plugin's author.", {}, kOtherSeed);
    }
    SUBCASE("a zip over 8 MB is refused by its size before downloading") {
        installOld(t);
        const std::string zip = goodZip();
        Server s(zip);
        Latest l = latest(zip);
        l.zipSize = kMaxZipBytes + 1;
        UserError err;
        CHECK_FALSE(stage(s.http, entry(), l, t.str(), kApp, nullptr, &err, roomy()));
        CHECK(err.message == "The download is damaged.");
        CHECK(s.sent.empty());
        CHECK(incomingEmpty(t));
    }
    SUBCASE("a zip that claims to be small but is over 8 MB when received") {
        std::string big(kMaxZipBytes + 1, '\0');
        uint64_t x = 88172645463325252ull;  // xorshift: doesn't compress
        for (auto& c : big) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            c = static_cast<char>(x);
        }
        const std::string zip = makeZip({{"plugin.json", manifest()}, {"main.js", "x"}, {"noise.bin", big}});
        Latest l = latest(zip);
        l.zipSize = 1000;
        checkRefused(t, zip, "The download is damaged.", l);
    }
    SUBCASE("a download URL off GitHub") {
        const std::string zip = goodZip();
        Latest l = latest(zip);
        for (const std::string url : {"http://github.com/a/plugin.zip", "https://evil.com/plugin.zip",
                                      "https://github.com.evil.com/plugin.zip", "https://github.com@evil.com/plugin.zip",
                                      "https://evilgithubusercontent.com/plugin.zip", "ftp://github.com/x"}) {
            CAPTURE(url);
            installOld(t);
            Server s(zip);
            Latest bad = l;
            bad.zipUrl = url;
            UserError err;
            CHECK_FALSE(stage(s.http, entry(), bad, t.str(), kApp, nullptr, &err, roomy()));
            CHECK(err.message == "The download is damaged.");
            CHECK(s.sent.empty());
            bad = l;
            bad.sigUrl = url;
            CHECK_FALSE(stage(s.http, entry(), bad, t.str(), kApp, nullptr, &err, roomy()));
            CHECK(s.sent.empty());
            CHECK(incomingEmpty(t));
        }
    }
    SUBCASE("a ../ entry") {
        checkRefused(t, makeZip({{"plugin.json", manifest()}, {"main.js", "x"}, {"../evil.js", "x"}}),
                     "The download is damaged.");
        CHECK_FALSE(fs::exists(t.path / ".incoming/evil.js"));
        CHECK_FALSE(fs::exists(t.path / "evil.js"));
    }
    SUBCASE("an absolute entry") {
        std::string zip = makeZip({{"plugin.json", manifest()}, {"main.js", "x"}, {"_tmp/evil.js", "x"}});
        for (size_t p = 0; (p = zip.find("_tmp/evil.js", p)) != std::string::npos;) zip[p] = '/';  // miniz won't write it
        checkRefused(t, zip, "The download is damaged.");
    }
    SUBCASE("a symlink entry") {
        checkRefused(t, makeZip({{"plugin.json", manifest()}, {"main.js", "x"}, {"link", "/etc/passwd"}}, {"link"}),
                     "The download is damaged.");
    }
    SUBCASE("a tree over 32 MB") {
        checkRefused(t, makeZip({{"plugin.json", manifest()}, {"main.js", "x"},
                                 {"zeros.bin", std::string(kMaxTreeBytes + 1, '\0')}}),
                     "The download is damaged.");
    }
    SUBCASE("id mismatch") {
        checkRefused(t, makeZip({{"plugin.json", manifest("1.2.0", json::array(), "other")}, {"main.js", "x"}}),
                     "It isn't the plugin the catalog lists.");
    }
    SUBCASE("version not equal to the tag") {
        checkRefused(t, goodZip("1.1.0"), "The release's version doesn't match its plugin.json.");
    }
    SUBCASE("minApp too new") {
        checkRefused(t, makeZip({{"plugin.json", manifest("1.2.0", json::array(), "feed-riffle", "2.0.0")},
                                 {"main.js", "x"}}),
                     "It needs app 2.0.0.");
    }
    SUBCASE("no plugin.json, or main missing") {
        checkRefused(t, makeZip({{"main.js", "x"}}), "The download is damaged.");
        checkRefused(t, makeZip({{"plugin.json", manifest()}}), "The download is damaged.");
    }
    SUBCASE("not enough space") {
        installOld(t);
        const std::string zip = goodZip();
        Server s(zip);
        DownloadHooks h = roomy();
        int64_t asked = 0;
        h.freeSpace = [&](const std::string&) { return asked = 3 * static_cast<int64_t>(zip.size()) + (16 << 20) - 1; };
        UserError err;
        CHECK_FALSE(stage(s.http, entry(), latest(zip), t.str(), kApp, nullptr, &err, h));
        CHECK(asked > 0);
        CHECK(err.message == "Not enough space.");
        CHECK(s.sent.empty());
        CHECK(incomingEmpty(t));
        h.freeSpace = [&](const std::string&) { return 3 * static_cast<int64_t>(zip.size()) + (16 << 20); };
        CHECK(stage(s.http, entry(), latest(zip), t.str(), kApp, nullptr, &err, h));
    }
    SUBCASE("offline") {
        installOld(t);
        FnHttp http([](const HttpRequest&) {
            HttpResponse r;
            r.curlCode = 6;
            return r;
        });
        DownloadHooks h = roomy();
        h.probe = [] { return NetState{false, true}; };
        UserError err;
        CHECK_FALSE(stage(http, entry(), latest(goodZip()), t.str(), kApp, nullptr, &err, h));
        CHECK(err.message == "No Wi-Fi.");
        CHECK(incomingEmpty(t));
    }
}

TEST_CASE("catalog install: added lists only the new permissions") {
    TempDir t;
    writeFile(t.path / "feed-riffle/plugin.json",
              manifest("1.0.0", json::array({"library:read", "net:a.example.org"})));
    writeFile(t.path / "feed-riffle/main.js", "x");
    UserError err;
    auto same = stageZip(t, goodZip("1.2.0", json::array({"net:a.example.org", "library:read"})), &err);
    REQUIRE(same);
    CHECK(same->added.empty());
    auto more = stageZip(t, goodZip("1.2.0", json::array({"library:read", "net:a.example.org", "zotero:tags",
                                                          "net:b.example.org"})),
                         &err);
    REQUIRE_MESSAGE(more, err.details);
    CHECK(more->added == std::vector<Permission>{Permission::Net, Permission::Tags});
    auto fewer = stageZip(t, goodZip("1.2.0", json::array()), &err);
    REQUIRE(fewer);
    CHECK(fewer->added.empty());
}

TEST_CASE("catalog install: [Review Focus] commit keeps data/, and a zip's own data/ never overwrites it") {
    TempDir t;
    installOld(t);
    UserError err;
    REQUIRE(stageZip(t, makeZip({{"plugin.json", manifest()}, {"main.js", "// v1.2.0"},
                                 {"data/notes.txt", "theirs"}, {"DATA/evil.txt", "theirs"}, {"Data/", ""}}),
                     &err));
    CHECK_FALSE(fs::exists(t.path / ".incoming/feed-riffle/data"));
    CHECK_FALSE(fs::exists(t.path / ".incoming/feed-riffle/DATA"));
    REQUIRE(commit(t.str(), "feed-riffle", &err));
    CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.2.0");
    CHECK(readFile(t.path / "feed-riffle/data/notes.txt") == "mine");
    CHECK_FALSE(fs::exists(t.path / "feed-riffle/data/evil.txt"));
    CHECK_FALSE(fs::exists(t.path / ".previous/feed-riffle/data"));
    CHECK(previousVersion(t.str(), "feed-riffle") == "1.0.0");
}

TEST_CASE("catalog install: its storage and log stay with it across update and roll back") {
    TempDir t;
    installOld(t);
    writeFile(t.path / "feed-riffle/storage.db", "kv");
    writeFile(t.path / "feed-riffle/storage.db-wal", "wal");
    writeFile(t.path / "feed-riffle/plugin.log", "log");
    UserError err;
    REQUIRE(stageZip(t, makeZip({{"plugin.json", manifest()}, {"main.js", "// v1.2.0"}, {"storage.db", "theirs"},
                                 {"PLUGIN.LOG", "theirs"}}),
                     &err));
    CHECK_FALSE(fs::exists(t.path / ".incoming/feed-riffle/storage.db"));
    REQUIRE(commit(t.str(), "feed-riffle", &err));
    CHECK(readFile(t.path / "feed-riffle/storage.db") == "kv");
    CHECK(readFile(t.path / "feed-riffle/storage.db-wal") == "wal");
    CHECK(readFile(t.path / "feed-riffle/plugin.log") == "log");
    CHECK_FALSE(fs::exists(t.path / ".previous/feed-riffle/storage.db"));
    REQUIRE(rollBack(t.str(), "feed-riffle", &err));
    CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.0.0");
    CHECK(readFile(t.path / "feed-riffle/storage.db") == "kv");
    CHECK_FALSE(fs::exists(t.path / ".previous/feed-riffle/storage.db"));
}

TEST_CASE("catalog install: one previous version is kept, and rollBack swaps both ways") {
    TempDir t;
    installOld(t);
    UserError err;
    REQUIRE(stageZip(t, goodZip("1.1.0"), &err, kSeed, latest(goodZip("1.1.0"), {1, 1, 0})));
    REQUIRE(commit(t.str(), "feed-riffle", &err));
    REQUIRE(stageZip(t, goodZip("1.2.0"), &err));
    REQUIRE(commit(t.str(), "feed-riffle", &err));
    CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.2.0");
    CHECK(previousVersion(t.str(), "feed-riffle") == "1.1.0");
    CHECK(std::distance(fs::directory_iterator(t.path / ".previous"), fs::directory_iterator{}) == 1);

    REQUIRE(rollBack(t.str(), "feed-riffle", &err));
    CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.1.0");
    CHECK(readFile(t.path / "feed-riffle/data/notes.txt") == "mine");
    CHECK(previousVersion(t.str(), "feed-riffle") == "1.2.0");
    CHECK_FALSE(fs::exists(t.path / ".previous/feed-riffle/data"));
    CHECK(incomingEmpty(t));

    REQUIRE(rollBack(t.str(), "feed-riffle", &err));
    CHECK(readFile(t.path / "feed-riffle/main.js") == "// v1.2.0");
    CHECK(readFile(t.path / "feed-riffle/data/notes.txt") == "mine");
    CHECK(previousVersion(t.str(), "feed-riffle") == "1.1.0");

    TempDir none;
    installOld(none);
    CHECK_FALSE(rollBack(none.str(), "feed-riffle", &err));  // nothing kept
    CHECK(readFile(none.path / "feed-riffle/main.js") == "// v1.0.0");
}

TEST_CASE("catalog install: [Review Focus] repairInterrupted after a kill mid-swap leaves one working plugin") {
    TempDir t;
    installOld(t);
    UserError err;
    REQUIRE(stageZip(t, goodZip(), &err));
    const fs::path id = t.path / "feed-riffle", prev = t.path / ".previous/feed-riffle";
    fs::create_directories(t.path / ".previous");

    SUBCASE("killed after <id> was moved aside") {
        writeFile(swapMarker(t.str(), "feed-riffle"), "feed-riffle");  // as swapIn writes it
        fs::rename(id, prev);
        repairInterrupted(t.str());
        CHECK(readFile(id / "main.js") == "// v1.0.0");
    }
    SUBCASE("killed after the new code was moved in, before data/ followed") {
        writeFile(swapMarker(t.str(), "feed-riffle"), "feed-riffle");  // as swapIn writes it
        fs::rename(id, prev);
        fs::rename(t.path / ".incoming/feed-riffle", id);
        repairInterrupted(t.str());
        CHECK(readFile(id / "main.js") == "// v1.2.0");
        CHECK(previousVersion(t.str(), "feed-riffle") == "1.0.0");
    }
    SUBCASE("a staged update never committed") {
        repairInterrupted(t.str());
        CHECK(readFile(id / "main.js") == "// v1.0.0");
    }
    CHECK(readFile(id / "data/notes.txt") == "mine");
    CHECK_FALSE(fs::exists(prev / "data"));
    CHECK_FALSE(fs::exists(t.path / ".incoming"));
    const auto scan = scanPlugins(t.str(), kApp);
    REQUIRE(scan.size() == 1);
    CHECK(scan[0].problem.empty());
}

TEST_CASE("catalog install: a plugin deleted by hand stays deleted, its kept version and state gone") {
    TempDir t;
    installOld(t);
    UserError err;
    REQUIRE(stageZip(t, goodZip(), &err));
    REQUIRE(commit(t.str(), "feed-riffle", &err));  // 1.0.0 kept in .previous
    CHECK_FALSE(fs::exists(swapMarker(t.str(), "feed-riffle")));
    writeFile(t.path / ".state.json", R"({"plugins":{"feed-riffle":{"enabled":true,"everEnabled":true}}})");
    fs::remove_all(t.path / "feed-riffle");  // over USB
    repairInterrupted(t.str());
    CHECK_FALSE(fs::exists(t.path / "feed-riffle"));
    CHECK_FALSE(fs::exists(t.path / ".previous/feed-riffle"));
    CHECK(readFile(t.path / ".state.json").find("feed-riffle") == std::string::npos);
    CHECK(scanPlugins(t.str(), kApp).empty());

    // With no kept version either: its state goes all the same.
    writeFile(t.path / ".state.json", R"({"plugins":{"other":{"enabled":true,"everEnabled":true}}})");
    repairInterrupted(t.str());
    CHECK(readFile(t.path / ".state.json").find("other") == std::string::npos);
}

TEST_CASE("catalog install: removeFiles deletes everything for that id and nothing else") {
    TempDir t;
    installOld(t);
    UserError err;
    REQUIRE(stageZip(t, goodZip(), &err));
    REQUIRE(commit(t.str(), "feed-riffle", &err));
    writeFile(t.path / "other/plugin.json", manifest("1.0.0", json::array(), "other"));
    writeFile(t.path / "other/main.js", "x");
    writeFile(t.path / "other/data/keep.txt", "keep");
    writeFile(t.path / ".previous/other/plugin.json", manifest("0.9.0", json::array(), "other"));
    writeFile(t.path / ".previous/other/main.js", "x");
    writeFile(t.path / "feed-riffle-2/plugin.json", "{}");
    Cache c = loadCache(t.str());
    c.fromCatalog.insert("other");
    REQUIRE(saveCache(t.str(), c));

    REQUIRE(removeFiles(t.str(), "feed-riffle", &err));
    CHECK_FALSE(fs::exists(t.path / "feed-riffle"));
    CHECK_FALSE(fs::exists(t.path / ".previous/feed-riffle"));
    CHECK(loadCache(t.str()).fromCatalog == std::set<std::string>{"other"});
    CHECK(readFile(t.path / "other/data/keep.txt") == "keep");
    CHECK(previousVersion(t.str(), "other") == "0.9.0");
    CHECK(fs::exists(t.path / "feed-riffle-2/plugin.json"));
    CHECK(incomingEmpty(t));

    CHECK_FALSE(removeFiles(t.str(), "../other", &err));  // not an id
    CHECK_FALSE(commit(t.str(), "..", &err));
    CHECK(fs::exists(t.path / "other/main.js"));
}

TEST_CASE("catalog install: [Review Focus] a zip's data/ in disguise never reaches the user's data/") {
    TempDir t;
    UserError err;
    SUBCASE("./data/x and other . or empty parts are refused") {
        for (const std::string name : {"./data/notes.txt", "./DATA/notes.txt", "a/./b.js", "a//b.js", "./main2.js",
                                       "a/.../b.js", "a/ /b.js"}) {
            CAPTURE(name);
            checkRefused(t, makeZip({{"plugin.json", manifest()}, {"main.js", "x"}, {name, "theirs"}}),
                         "The download is damaged.");
        }
    }
    SUBCASE("Data/x, data./x and \"data /x\" are left out, as FAT would fold them into data/") {
        installOld(t);
        REQUIRE(stageZip(t, makeZip({{"plugin.json", manifest()}, {"main.js", "// v1.2.0"}, {"Data/notes.txt", "theirs"},
                                     {"data./notes.txt", "theirs"}, {"data. ./notes.txt", "theirs"},
                                     {"data /evil.txt", "theirs"}, {"datas/ok.txt", "fine"}}),
                         &err));
        std::set<std::string> names;
        for (const auto& e : fs::directory_iterator(t.path / ".incoming/feed-riffle"))
            names.insert(e.path().filename().string());
        CHECK(names == std::set<std::string>{"datas", "main.js", "plugin.json"});
        REQUIRE(commit(t.str(), "feed-riffle", &err));
        CHECK(readFile(t.path / "feed-riffle/data/notes.txt") == "mine");
        CHECK(readFile(t.path / "feed-riffle/datas/ok.txt") == "fine");
    }
}

TEST_CASE("catalog install: [Review Focus] a data/ stranded in .previous is moved back, never deleted") {
    TempDir t;
    installOld(t);
    UserError err;
    REQUIRE(stageZip(t, goodZip("1.1.0"), &err, kSeed, latest(goodZip("1.1.0"), {1, 1, 0})));
    REQUIRE(commit(t.str(), "feed-riffle", &err));
    const fs::path cur = t.path / "feed-riffle", prev = t.path / ".previous/feed-riffle";
    fs::rename(cur / "data", prev / "data");  // as after a failed step 4
    REQUIRE(stageZip(t, goodZip(), &err));

    SUBCASE("commit moves it back first") {
        REQUIRE(commit(t.str(), "feed-riffle", &err));
        CHECK(readFile(cur / "main.js") == "// v1.2.0");
        CHECK(readFile(cur / "data/notes.txt") == "mine");
    }
    SUBCASE("with a data/ in <id> too, commit and rollBack refuse and change nothing") {
        writeFile(cur / "data/fresh.txt", "new");
        CHECK_FALSE(commit(t.str(), "feed-riffle", &err));
        CHECK_FALSE(rollBack(t.str(), "feed-riffle", &err));
        CHECK(readFile(prev / "data/notes.txt") == "mine");
        CHECK(readFile(cur / "data/fresh.txt") == "new");
        CHECK(readFile(cur / "main.js") == "// v1.1.0");
        CHECK(previousVersion(t.str(), "feed-riffle") == "1.0.0");
        CHECK(fs::exists(t.path / ".incoming/feed-riffle/plugin.json"));
    }
    SUBCASE("repairOne moves it back and leaves .incoming alone") {
        repairOne(t.str(), "feed-riffle");
        CHECK(readFile(cur / "data/notes.txt") == "mine");
        CHECK_FALSE(fs::exists(prev / "data"));
        CHECK(fs::exists(t.path / ".incoming/feed-riffle/plugin.json"));
    }
}

TEST_CASE("catalog install: the space check counts the unpacked tree, and entries are capped") {
    TempDir t;
    installOld(t);
    const std::string zip = makeZip({{"plugin.json", manifest()}, {"main.js", "x"},
                                     {"zeros.bin", std::string(20 << 20, '\0')}});
    Server s(zip);
    DownloadHooks h = roomy();
    h.freeSpace = [&](const std::string&) { return int64_t{17} << 20; };  // enough for the zip, not the tree
    UserError err;
    CHECK_FALSE(stage(s.http, entry(), latest(zip), t.str(), kApp, nullptr, &err, h));
    CHECK(err.message == "Not enough space.");
    CHECK(incomingEmpty(t));

    std::vector<std::pair<std::string, std::string>> many = {{"plugin.json", manifest()}, {"main.js", "x"}};
    for (unsigned i = 0; i < kMaxZipEntries; ++i) many.push_back({"f/" + std::to_string(i), ""});
    checkRefused(t, makeZip(many), "The download is damaged.");
}
