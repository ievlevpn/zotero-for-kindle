#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "docs/guarded_engine.h"
#include "fake_engine.h"
#include "plugins/fulltext.h"

using namespace zk;
namespace fs = std::filesystem;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}; }

std::string freshDir(const std::string& name) {
    const std::string d = "/tmp/zk-fulltext-" + name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

AttachmentRecord local(const std::string& key, const std::string& type, const std::string& path, const std::string& md5 = "abc123") {
    AttachmentRecord a;
    a.key = key;
    a.contentType = type;
    a.md5 = md5;
    a.linkMode = "imported_file";
    a.localPath = path;
    a.state = AttachmentRecord::State::Local;
    return a;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("fulltext: an EPUB fixture's text in reading order") {
    const std::string plugins = freshDir("epub");
    UserError err;
    auto text = extractFullText(local("BOOK0001", "application/epub+zip", "tests/data/epub/pride-and-prejudice.epub"), plugins, env(), &err);
    REQUIRE_MESSAGE(text, err.message);
    const size_t first = text->find("It is a truth universally acknowledged");
    REQUIRE(first != std::string::npos);
    const size_t later = text->find("Elizabeth", first);
    CHECK(later != std::string::npos);
    CHECK(text->size() > 500000);
    CHECK(has(*text, "\n\n"));
    CHECK_FALSE(has(*text, "\n\n\n"));
    CHECK_FALSE(has(*text, "\f"));
}

TEST_CASE("fulltext: a PDF fixture's text page by page") {
    const std::string plugins = freshDir("pdf");
    UserError err;
    auto text = extractFullText(local("PAPER001", "application/pdf", "tests/data/pdf/sample.pdf"), plugins, env(), &err);
    REQUIRE_MESSAGE(text, err.message);
    std::vector<std::string> pages;
    std::stringstream ss(*text);
    for (std::string p; std::getline(ss, p, '\f');) pages.push_back(p);
    REQUIRE(pages.size() == 3);
    CHECK(has(pages[1], "Hamiltonian"));
    CHECK_FALSE(has(pages[0], "Hamiltonian"));
}

TEST_CASE("fulltext: a file not on the Kindle is an error, not empty text") {
    const std::string plugins = freshDir("missing");
    UserError err;
    AttachmentRecord remote = local("REMOTE01", "application/pdf", "tests/data/pdf/sample.pdf");
    remote.state = AttachmentRecord::State::Remote;
    CHECK_FALSE(extractFullText(remote, plugins, env(), &err));
    CHECK(has(err.message, "isn't on the Kindle"));

    err = {};
    CHECK_FALSE(extractFullText(local("GONE0001", "application/pdf", "/tmp/zk-fulltext-nope.pdf"), plugins, env(), &err));
    CHECK(has(err.message, "isn't on the Kindle"));

    err = {};
    AttachmentRecord linked = local("LINK0001", "application/pdf", "tests/data/pdf/sample.pdf");
    linked.linkMode = "linked_file";
    CHECK_FALSE(extractFullText(linked, plugins, env(), &err));
    CHECK(has(err.message, "linked"));

    err = {};
    CHECK_FALSE(extractFullText(local("HTML0001", "text/html", "tests/data/pdf/sample.pdf"), plugins, env(), &err));
    CHECK(has(err.message, "only for EPUB and PDF"));

    err = {};  // a damaged file: the engine's own message
    const std::string bad = plugins + "/bad.pdf";
    std::ofstream(bad) << "not a pdf";
    CHECK_FALSE(extractFullText(local("BAD00001", "application/pdf", bad), plugins, env(), &err));
    CHECK_FALSE(err.message.empty());
}

TEST_CASE("fulltext: a second call is served from the cache") {
    const std::string plugins = freshDir("cache");
    const std::string copy = plugins + "/copy.pdf";
    fs::copy_file("tests/data/pdf/sample.pdf", copy);
    UserError err;
    auto a = local("CACHE001", "application/pdf", copy, "md5one");
    auto first = extractFullText(a, plugins, env(), &err);
    REQUIRE_MESSAGE(first, err.message);
    CHECK(fs::exists(plugins + "/.cache/fulltext/CACHE001-md5one.txt"));
    CHECK_FALSE(fs::exists(plugins + "/.cache/fulltext/CACHE001-md5one.txt.tmp"));

    // The file is now unreadable as a PDF: only the cache can answer.
    std::ofstream(copy, std::ios::trunc) << "garbage";
    auto second = extractFullText(a, plugins, env(), &err);
    REQUIRE_MESSAGE(second, err.message);
    CHECK(*second == *first);

    // A new md5 for the same attachment: the stale entry goes when the new one is written.
    fs::copy_file("tests/data/pdf/single-column.pdf", copy, fs::copy_options::overwrite_existing);
    a.md5 = "md5two";
    auto third = extractFullText(a, plugins, env(), &err);
    REQUIRE_MESSAGE(third, err.message);
    CHECK(*third != *first);
    CHECK(fs::exists(plugins + "/.cache/fulltext/CACHE001-md5two.txt"));
    CHECK_FALSE(fs::exists(plugins + "/.cache/fulltext/CACHE001-md5one.txt"));

    // Another attachment's entry is left alone.
    std::ofstream(plugins + "/.cache/fulltext/OTHER001-x.txt") << "x";
    a.md5 = "md5three";
    REQUIRE(extractFullText(a, plugins, env(), &err));
    CHECK(fs::exists(plugins + "/.cache/fulltext/OTHER001-x.txt"));
}

namespace {

struct FailingEngine : test::FakeEngine {
    bool throws = false;
    bool fullText(std::string& out, UserError* err) override {
        out = "partial";
        if (throws) throw std::bad_alloc();
        if (err) *err = UserError{ErrorKind::Other, "This PDF's text can't be read.", "fake"};
        return false;
    }
};

EngineLoader failing(bool throws) {
    return [throws](DocType, const EngineEnv&, UserError*) -> std::unique_ptr<DocumentEngine> {
        auto e = std::make_unique<FailingEngine>();
        e->throws = throws;
        return std::make_unique<GuardedEngine>(std::move(e));
    };
}

bool cacheEmpty(const std::string& plugins) {
    const fs::path d = fs::path(plugins) / ".cache" / "fulltext";
    return !fs::exists(d) || fs::is_empty(d);
}

}  // namespace

TEST_CASE("fulltext: an engine failure is an error and is never cached") {
    for (bool throws : {false, true}) {
        const std::string plugins = freshDir("fail");
        UserError err;
        auto a = local("FAIL0001", "application/pdf", "tests/data/pdf/sample.pdf");
        CHECK_FALSE(extractFullText(a, plugins, env(), &err, failing(throws)));
        CHECK_FALSE(err.message.empty());
        CHECK(cacheEmpty(plugins));
        // The real engine still works afterwards: nothing wrong was left behind.
        CHECK(extractFullText(a, plugins, env(), &err));
    }
}

TEST_CASE("fulltext: no checksum, no cache") {
    const std::string plugins = freshDir("nomd5");
    UserError err;
    auto text = extractFullText(local("NOSUM001", "application/pdf", "tests/data/pdf/sample.pdf", ""), plugins, env(), &err);
    REQUIRE_MESSAGE(text, err.message);
    CHECK(cacheEmpty(plugins));
}

TEST_CASE("fulltext: an unreadable cache folder doesn't make extraction throw") {
    const std::string plugins = freshDir("badcache");
    std::ofstream(plugins + "/.cache") << "a file where the folder should be";
    UserError err;
    auto text = extractFullText(local("NODIR001", "application/pdf", "tests/data/pdf/sample.pdf"), plugins, env(), &err);
    CHECK(text);
}

TEST_CASE("fulltext: the cache keeps at most 64 MB, dropping the least recently used entries") {
    const std::string plugins = freshDir("lru");
    const fs::path dir = fs::path(plugins) / ".cache" / "fulltext";
    fs::create_directories(dir);
    const auto now = fs::file_time_type::clock::now();
    auto entry = [&](const std::string& name, int hoursAgo) {
        std::ofstream(dir / name) << "";
        fs::resize_file(dir / name, 30u << 20);  // sparse
        fs::last_write_time(dir / name, now - std::chrono::hours(hoursAgo));
    };
    entry("KEYA-ma.txt", 3);
    entry("KEYB-mb.txt", 2);
    entry("KEYC-mc.txt", 1);
    UserError err;
    // A hit on the oldest makes it the most recently used.
    const std::string copyA = plugins + "/a.pdf";
    fs::copy_file("tests/data/pdf/sample.pdf", copyA);
    REQUIRE(extractFullText(local("KEYA", "application/pdf", copyA, "ma"), plugins, env(), &err));
    CHECK(fs::last_write_time(dir / "KEYA-ma.txt") > now - std::chrono::minutes(1));
    // A new entry takes the cache over 64 MB: the least recently used go until it fits.
    const std::string copy = plugins + "/copy.pdf";
    fs::copy_file("tests/data/pdf/sample.pdf", copy);
    REQUIRE_MESSAGE(extractFullText(local("NEW1", "application/pdf", copy, "mn"), plugins, env(), &err), err.message);
    CHECK(fs::exists(dir / "NEW1-mn.txt"));
    CHECK(fs::exists(dir / "KEYA-ma.txt"));
    CHECK(fs::exists(dir / "KEYC-mc.txt"));
    CHECK_FALSE(fs::exists(dir / "KEYB-mb.txt"));
    uint64_t total = 0;
    for (const auto& e : fs::directory_iterator(dir)) total += fs::file_size(e.path());
    CHECK(total <= kMaxFullTextCacheBytes);
}
