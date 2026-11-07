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

// All links of the first `pages` pages, described.
std::vector<LinkInfo> allLinks(DocumentEngine& e, int pages) {
    std::vector<LinkInfo> out;
    for (int p = 0; p < std::min(pages, e.pageCount()); ++p) {
        std::vector<int> seen;
        for (const LinkBox& box : e.links(p)) {
            if (std::find(seen.begin(), seen.end(), box.index) != seen.end()) continue;
            seen.push_back(box.index);
            if (auto info = e.link(p, box.index)) out.push_back(*info);
        }
    }
    return out;
}

const LinkInfo* withMark(const std::vector<LinkInfo>& links, const std::string& mark) {
    for (const auto& l : links)
        if (l.mark == mark) return &l;
    return nullptr;
}

}  // namespace

TEST_CASE("links: PDF references show the cited entry and only it") {
    auto pdf = open(DocType::Pdf, "tests/data/pdf/links.pdf");
    const Layout l;
    for (const auto& b : pdf->links(0)) {
        CHECK(b.rect.w > 10);
        CHECK(b.rect.x >= 0);
        CHECK(b.rect.x + b.rect.w <= l.width);
    }
    const auto links = allLinks(*pdf, 1);
    REQUIRE(links.size() == 5);
    auto ref = [&](const std::string& start) -> const LinkInfo* {
        for (const auto& l : links)
            if (l.kind == LinkInfo::Kind::Reference && l.text.rfind(start, 0) == 0) return &l;
        return nullptr;
    };
    // A destination exactly at the entry.
    const LinkInfo* one = ref("[1] M. Raissi");
    REQUIRE(one);
    CHECK(one->targetPage == 1);
    CHECK(one->label == "Page 2");
    CHECK(one->text.find("686-707") != std::string::npos);
    CHECK(one->text.find("[2]") == std::string::npos);
    // A line above the entry (hyperref), alphabetic labels, hanging indent, no gap between entries.
    const LinkInfo* fv = ref("[FV07] P. Friz");
    REQUIRE(fv);
    CHECK(fv->text.find("Cambridge University Press, 2010.") != std::string::npos);
    CHECK(fv->text.find("Wadsworth") == std::string::npos);  // the previous entry's last line
    CHECK(fv->text.find("Hai14") == std::string::npos);      // the next entry
    // Author-year.
    const LinkInfo* smith = ref("Smith, J. (2020)");
    REQUIRE(smith);
    CHECK(smith->text.find("Taylor") == std::string::npos);
    CHECK(smith->text.find("269-504") == std::string::npos);
    // The right column of a two-column list; a vertical stamp in the margin.
    const LinkInfo* b2 = ref("[B2] The right column entry we");
    REQUIRE(b2);
    CHECK(b2->text.find("cite, continued here.") != std::string::npos);
    CHECK(b2->text.find("left") == std::string::npos);
    CHECK(b2->text.find("[B3]") == std::string::npos);
    CHECK(b2->text.find("arXiv") == std::string::npos);
    bool web = false;
    for (const auto& l : links) web |= l.kind == LinkInfo::Kind::Web && l.url == "https://example.org/pinns";
    CHECK(web);
    CHECK(pdf->links(1).empty());
}

TEST_CASE("links: EPUB footnotes as Gutenberg, Pandoc and Standard Ebooks make them") {
    auto epub = open(DocType::Epub, "tests/data/epub/links-patterns.epub");
    const auto links = allLinks(*epub, epub->pageCount());
    const LinkInfo* gutenberg = withMark(links, "26");
    REQUIRE(gutenberg);
    CHECK(gutenberg->kind == LinkInfo::Kind::Note);
    CHECK(gutenberg->label == "Footnote 26");
    CHECK(gutenberg->text == "The senators were obliged to hold a third of their land in Italy.");
    const LinkInfo* pandoc = withMark(links, "1");
    REQUIRE(pandoc);
    CHECK(pandoc->kind == LinkInfo::Kind::Note);
    bool pandocText = false, endnote = false;
    for (const auto& l : links) {
        pandocText |= l.kind == LinkInfo::Kind::Note && l.text == "Pandoc's note text.";
        endnote |= l.kind == LinkInfo::Kind::Note && l.text == "An endnote at the back.";
    }
    CHECK(pandocText);
    CHECK(endnote);
    // Not notes: a chapter link, a printed-page anchor, the links back from notes.
    REQUIRE(withMark(links, "II"));
    CHECK(withMark(links, "II")->kind == LinkInfo::Kind::Jump);
    REQUIRE(withMark(links, "23"));
    CHECK(withMark(links, "23")->kind == LinkInfo::Kind::Jump);
    REQUIRE(withMark(links, "return"));
    CHECK(withMark(links, "return")->kind == LinkInfo::Kind::Jump);
    for (const auto& l : links)
        if (l.mark.find("\xE2\x86\xA9") != std::string::npos) CHECK(l.kind == LinkInfo::Kind::Jump);
}

TEST_CASE("links: Gibbon's Gutenberg footnotes open as notes, with their own text") {
    auto epub = open(DocType::Epub, "tests/data/epub/gibbon-footnotes.epub");
    // The main text (the first ~110 pages are the contents: rightly jumps).
    int notes = 0, jumps = 0;
    for (int p = 120; p < 400; ++p) {
        std::vector<int> seen;
        for (const LinkBox& box : epub->links(p)) {
            if (std::find(seen.begin(), seen.end(), box.index) != seen.end()) continue;
            seen.push_back(box.index);
            if (auto l = epub->link(p, box.index); l && l->mark != "return")  // each note's way back: a jump
                (l->kind == LinkInfo::Kind::Note ? notes : jumps)++;
        }
    }
    MESSAGE("pages 121-400: " << notes << " notes, " << jumps << " jumps");
    CHECK(notes > 100);
    CHECK(notes > 4 * jumps);
    bool senators = false;
    for (const auto& l : allLinks(*epub, 600))
        if (l.kind == LinkInfo::Kind::Note && l.text.rfind("The senators were obliged to have one third", 0) == 0) {
            senators = true;
            CHECK(l.text.find("provinces.") != std::string::npos);
            CHECK(l.text.find("return") == std::string::npos);
            break;
        }
    CHECK(senators);
}

TEST_CASE("links: Standard Ebooks endnotes (epub:type noteref) are notes") {
    auto epub = open(DocType::Epub, "tests/data/epub/moby-dick-se.epub");
    auto note = firstLink(*epub, LinkInfo::Kind::Note, epub->pageCount());
    REQUIRE(note);
    CHECK(note->second.text == "See subsequent chapters for something more on this head.");
}

TEST_CASE("links: an EPUB link to another chapter is a jump") {
    auto epub = open(DocType::Epub, "tests/data/epub/the-prince-notes.epub");
    auto jump = firstLink(*epub, LinkInfo::Kind::Jump, 40);
    REQUIRE(jump);
    CHECK(jump->second.targetPage >= 0);
    CHECK_FALSE(jump->second.target.value.empty());
}

TEST_CASE("links: two columns list each link once") {
    UserError e;
    auto epub = loadEngine(DocType::Epub, env(), &e);
    REQUIRE(epub->open("tests/data/epub/links-patterns.epub", &e));
    Layout wide;
    wide.width = 2400;
    wide.height = 1600;
    wide.columns = 2;
    epub->setLayout(wide);
    std::vector<int> indexes;
    for (const auto& b : epub->links(0)) indexes.push_back(b.index);
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    for (size_t i = 0; i < indexes.size(); ++i)
        if (auto a = epub->link(0, indexes[i]))
            for (size_t k = i + 1; k < indexes.size(); ++k)
                if (auto b = epub->link(0, indexes[k])) CHECK_FALSE((a->mark == b->mark && a->target.value == b->target.value));
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
    explicit ReaderRig(DocType type = DocType::Epub) {
        fillFixtureLibrary(lib, 10);
        row = *lib.rowFor("IT000003");
        r = std::make_unique<ReaderScreen>(row, type == DocType::Pdf ? "book.pdf" : "book.epub", type, [] {
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

TEST_CASE("reader links: the pen doesn't write under an open pop-up") {
    ReaderRig rig(DocType::Pdf);
    CHECK(static_cast<Screen&>(*rig.r).wantsSystemInk());
    rig.r->onGesture(tapAt({230, 420}), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->linkPopup().has_value(); }));
    CHECK_FALSE(static_cast<Screen&>(*rig.r).wantsSystemInk());
}

TEST_CASE("reader links: a late answer after a page turn is dropped") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({560, 420}), rig.nav);   // the jump
    rig.r->onGesture(tapAt({1100, 700}), rig.nav);  // and at once a page turn
    REQUIRE(rig.waitPage(4));
    for (int i = 0; i < 30; ++i) {
        rig.r->onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(rig.r->page() == 4);
    CHECK(rig.r->backLabel().value_or("").rfind("‹ Back to page 1", 0) == 0);  // the rig's own jump, not the link's
    CHECK_FALSE(rig.r->linkPopup());
}

TEST_CASE("reader links: a late answer doesn't open behind the menus") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({230, 420}), rig.nav);  // the note
    rig.r->onGesture(tapAt({618, 700}), rig.nav);  // the centre: the menus
    REQUIRE(rig.r->menusOpen());
    for (int i = 0; i < 30; ++i) {
        rig.r->onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_FALSE(rig.r->linkPopup());
}

TEST_CASE("reader links: a tiny note mark has a fingertip-sized target") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({302 + 28, 905 + 20}), rig.nav);  // ~2.5 mm off a 4 px dagger
    REQUIRE(rig.until([&] { return rig.r->linkPopup().has_value(); }));
    CHECK(rig.r->linkPopup()->kind == LinkInfo::Kind::Note);
}

TEST_CASE("reader links: the outer strip of the page still turns it, link or not") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({1200, 1220}), rig.nav);  // on the link, at the very edge
    REQUIRE(rig.waitPage(4));
    CHECK_FALSE(rig.r->linkPopup());
}

TEST_CASE("reader links: closing the pop-up repaints with the clean waveform") {
    ReaderRig rig;
    rig.r->onGesture(tapAt({230, 420}), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->linkPopup().has_value(); }));
    rig.nav.damages.clear();
    rig.r->onGesture(tapAt({600, 200}), rig.nav);
    REQUIRE_FALSE(rig.nav.damages.empty());
    CHECK(rig.nav.damages.back().waveform == Waveform::Quality);
}
