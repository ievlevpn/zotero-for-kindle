// Footnotes and links: where a page's links are, and what each one is (a note, a reference to
// show, a jump, a web address).
#include <doctest/doctest.h>

#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/reader.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/text.h"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace zk;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}; }

std::unique_ptr<DocumentEngine> open(DocType type, const std::string& path) {
    UserError e;
    auto engine = loadEngine(type, type == DocType::Epub ? env() : EngineEnv{}, &e);
    REQUIRE_MESSAGE(engine, e.details);
    REQUIRE(engine->open(path, &e));
    engine->setLayout(Layout{});
    return engine;
}

// The first link of `kind` in the first `pages` pages.
std::optional<std::pair<int, LinkInfo>> firstLink(DocumentEngine& e, LinkInfo::Kind kind, int pages) {
    for (int p = 0; p < std::min(pages, e.pageCount()); ++p)
        for (const LinkBox& box : e.links(p))
            if (auto info = e.link(p, box.index); info && info->kind == kind) return std::make_pair(p, *info);
    return std::nullopt;
}

}  // namespace

TEST_CASE("links: a PDF citation shows the text at its target, with the page to go to") {
    auto pdf = open(DocType::Pdf, "tests/data/pdf/links.pdf");
    const auto boxes = pdf->links(0);
    REQUIRE(boxes.size() == 2);
    const Layout l;
    for (const auto& b : boxes) {
        CHECK(b.rect.w > 10);
        CHECK(b.rect.x >= 0);
        CHECK(b.rect.x + b.rect.w <= l.width);
    }
    std::optional<LinkInfo> cite, web;
    for (const auto& b : boxes) {
        auto info = pdf->link(0, b.index);
        REQUIRE(info);
        (info->kind == LinkInfo::Kind::Reference ? cite : web) = info;
    }
    REQUIRE(cite);
    CHECK(cite->targetPage == 1);
    CHECK(cite->label == "Page 2");
    CHECK(cite->text.find("[1] M. Raissi") != std::string::npos);
    CHECK(cite->text.find("686-707") != std::string::npos);  // the whole entry ...
    CHECK(cite->text.find("[2]") == std::string::npos);       // ... and not the next one
    REQUIRE(web);
    CHECK(web->kind == LinkInfo::Kind::Web);
    CHECK(web->url == "https://example.org/pinns");
    CHECK(pdf->links(1).empty());
}

TEST_CASE("links: an EPUB footnote shows its text") {
    auto epub = open(DocType::Epub, "tests/data/epub/gibbon-footnotes.epub");
    auto note = firstLink(*epub, LinkInfo::Kind::Note, 120);
    REQUIRE(note);
    CHECK(note->second.text.size() > 10);
    CHECK(note->second.text.size() < 4000);
    CHECK(note->second.label.rfind("Footnote", 0) == 0);
    CHECK(note->second.targetPage >= 0);  // "Go to note"
}

TEST_CASE("links: Standard Ebooks endnotes (epub:type noteref) are notes") {
    auto epub = open(DocType::Epub, "tests/data/epub/moby-dick-se.epub");
    auto note = firstLink(*epub, LinkInfo::Kind::Note, epub->pageCount());
    REQUIRE(note);
    CHECK_FALSE(note->second.text.empty());
    CHECK(note->second.text.find("\xE2\x86\xA9") == std::string::npos);  // without the link back
    MESSAGE("endnote on page " << note->first + 1 << ": " << note->second.label << ": " << note->second.text.substr(0, 120));
}

TEST_CASE("links: an EPUB link to another chapter is a jump") {
    auto epub = open(DocType::Epub, "tests/data/epub/the-prince-notes.epub");
    auto jump = firstLink(*epub, LinkInfo::Kind::Jump, 40);
    REQUIRE(jump);
    CHECK(jump->second.targetPage >= 0);
    CHECK_FALSE(jump->second.target.value.empty());
}

TEST_CASE("links: pages without links have none, and bad indexes are nothing") {
    auto epub = open(DocType::Epub, "tests/data/epub/frankenstein-epub2-ncx.epub");
    CHECK_FALSE(epub->link(5, 999));
    auto pdf = open(DocType::Pdf, "tests/data/pdf/sample.pdf");
    CHECK(pdf->links(0).empty());
    CHECK_FALSE(pdf->link(0, 0));
}

// ---- The reader: a tap on a link ----

namespace {

using namespace zk::test;

struct ReaderRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    ItemRow row;
    std::unique_ptr<ReaderScreen> r;
    ReaderRig() {
        fillFixtureLibrary(lib, 10);
        row = *lib.rowFor("IT000003");
        r = std::make_unique<ReaderScreen>(row, "book.epub", DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = r.get();
        r->onShow(nav);
        REQUIRE(waitPage(0));
        FakeEngine probe;
        r->jumpTo(probe.positionOfPage(3));  // a page with links
        REQUIRE(waitPage(3));
        REQUIRE(until([&] { return !r->links().empty(); }));
    }
    bool until(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            r->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool waitPage(int page) { return until([&] { return r->ready() && r->page() == page && !r->turning(); }); }
};

}  // namespace

TEST_CASE("reader links: a footnote opens in a pop-up; a tap elsewhere closes it and stays") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({230, 420}), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->linkPopup().has_value(); }));
    CHECK(rig.r->linkPopup()->kind == LinkInfo::Kind::Note);
    CHECK(rig.r->linkPopup()->text.rfind("See Zosimus", 0) == 0);
    rig.r->onGesture(tapAt({1100, 300}), rig.nav);  // the right edge: closes, doesn't turn
    CHECK_FALSE(rig.r->linkPopup());
    for (int i = 0; i < 20; ++i) rig.r->onTick(rig.nav);
    CHECK(rig.r->page() == 3);
}

TEST_CASE("reader links: Go to note jumps there, with Back") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({230, 420}), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->linkPopup().has_value(); }));
    rig.r->onGesture(tapAt(rig.r->linkButtonRect().center()), rig.nav);
    REQUIRE(rig.waitPage(53));
    CHECK_FALSE(rig.r->linkPopup());
    CHECK(rig.r->backLabel()->rfind("‹ Back to page 4", 0) == 0);
}

TEST_CASE("reader links: a link to another chapter jumps at once, with Back") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({560, 420}), rig.nav);
    REQUIRE(rig.waitPage(23));
    CHECK_FALSE(rig.r->linkPopup());
    CHECK(rig.r->backLabel()->rfind("‹ Back to page 4", 0) == 0);
}

TEST_CASE("reader links: a web link shows its address, nothing to go to") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({860, 420}), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->linkPopup().has_value(); }));
    CHECK(rig.r->linkPopup()->url == "https://example.org/pinns");
    CHECK(rig.r->linkButtonRect().empty());
}

TEST_CASE("reader links: a tap beside a link still turns the page") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({1100, 700}), rig.nav);
    REQUIRE(rig.waitPage(4));
    CHECK_FALSE(rig.r->linkPopup());
}

TEST_CASE("reader links: the pop-ups match the reviewed goldens") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    ReaderRig rig;
    rig.r->onGesture(tapAt({230, 420}), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->linkPopup().has_value(); }));
    Canvas c(kPw5.width, kPw5.height);
    rig.r->draw(c);
    checkGolden(c, "tests/golden/link-note.png");
    rig.r->onGesture(tapAt({600, 200}), rig.nav);  // closes
    rig.r->onGesture(tapAt({860, 420}), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->linkPopup() && !rig.r->linkPopup()->url.empty(); }));
    Canvas c2(kPw5.width, kPw5.height);
    rig.r->draw(c2);
    checkGolden(c2, "tests/golden/link-web.png");
}
