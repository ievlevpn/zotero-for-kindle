// zk.reader.open (0.9): reading something that isn't in the library. The plugin side (when it may be
// called, the net permission, the promise), the preview cache, type sniffing, and the reader in
// preview mode.

#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <thread>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fixture_library.h"
#include "plugins/host.h"
#include "screens/annotation_bar.h"
#include "screens/preview.h"
#include "screens/preview_screen.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "store/library.h"
#include "ui/text.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
using Kind = PluginHostEvent::Kind;

namespace {

struct Host {
    FakePlugins fp;
    std::unique_ptr<PluginHost> host;
    std::deque<PluginHostEvent> got;

    explicit Host(const std::string& js, const std::vector<std::string>& perms = {}) {
        fp.add("p", js, perms);
        PluginHostDeps d = fp.deps();
        d.renderBudgetMs = 300;
        d.actionBudgetMs = 600;
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("p", true);
        host->start();
    }
    ~Host() { host->stop(); }

    std::optional<PluginHostEvent> next(Kind kind, int ms = 3000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            PluginHostEvent ev;
            while (host->poll(ev)) got.push_back(std::move(ev));
            for (auto it = got.begin(); it != got.end(); ++it)
                if (it->kind == kind) {
                    PluginHostEvent out = std::move(*it);
                    got.erase(it);
                    return out;
                }
            if (std::chrono::steady_clock::now() >= end) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // The text of the next tree's first block.
    std::string nextText() {
        auto ev = next(Kind::ScreenTree);
        REQUIRE(ev);
        REQUIRE(ev->tree);
        const Block& b = ev->tree->kind == Block::Kind::Column ? ev->tree->children.at(0) : *ev->tree;
        return plainText(b);
    }
};

// A screen whose render shows `said`; a tap on "go" calls zk.reader.open(doc of the tap's id).
const char* kScreen = R"JS(
let said = "nothing";
const docs = {
  pdf: { url: "https://arxiv.org/pdf/2401.01234v2", title: "A paper" },
  other: { url: "https://example.com/paper.pdf" },
  plain: { url: "http://arxiv.org/pdf/2401.01234v2" },
  page: { html: "<p>An article</p>", title: "An article", baseUrl: "https://example.com/a" },
  untitled: { html: "<p>An article</p>" },
  neither: { title: "Nothing" },
};
const tryOpen = (doc) => {
  try {
    zk.reader.open(doc).then(() => { said = "opened"; zk.ui.refresh(); }, (e) => { said = "rejected: " + e.message; zk.ui.refresh(); });
    return "asked";
  } catch (e) {
    return e.name + ": " + e.message;
  }
};
zk.screen("main", {
  title: "Reading",
  render: () => {
    if (said === "nothing") said = "render: " + tryOpen(docs.pdf);
    return zk.ui.column([zk.ui.text(said)]);
  },
  onEvent: (e) => {
    if (e.kind === "tap") said = tryOpen(docs[e.id]);
    if (e.kind === "reader") said = "reader closed: " + e.closed;
  },
});
zk.on("syncFinished", () => { said = "handler: " + tryOpen(docs.pdf); });
)JS";

}  // namespace

TEST_CASE("reader preview: zk.reader.open only answers the user; it needs https and the net permission for the host") {
    Host h(kScreen, {"net:arxiv.org"});
    const uint64_t s = h.host->openScreen("p", "main");
    CHECK(h.nextText().find("render: Error: zk.reader.open: only in answer to the user") == 0);
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "other"}});
    CHECK(h.nextText() == "PermissionError: zk.reader.open(\"https://example.com/paper.pdf\") needs \"net:example.com\" in plugin.json");
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "plain"}});
    CHECK(h.nextText().find("Error: zk.reader.open: only https:// URLs are allowed") == 0);
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "untitled"}});
    CHECK(h.nextText() == "TypeError: zk.reader.open: an html document needs a `title`");
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "neither"}});
    CHECK(h.nextText() == "TypeError: zk.reader.open: expected { url, title? } or { html, title, baseUrl? }");
    CHECK_FALSE(h.next(Kind::ReaderOpen, 100));  // none of them reached the screen
}

TEST_CASE("reader preview: a url opens over its screen; the promise resolves when the reader opens, the wait is free") {
    Host h(kScreen, {"net:arxiv.org"});
    const uint64_t s = h.host->openScreen("p", "main");
    h.nextText();
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "pdf"}});
    auto open = h.next(Kind::ReaderOpen);
    REQUIRE(open);
    CHECK(open->instance == s);
    CHECK(open->pluginId == "p");
    CHECK(open->value == json{{"url", "https://arxiv.org/pdf/2401.01234v2"}, {"title", "A paper"}});
    CHECK(h.nextText() == "asked");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));  // longer than the action's 600 ms: waiting is free
    h.host->answerReader(open->requestId, "");
    CHECK(h.nextText() == "opened");
    h.host->screenEvent(s, {{"kind", "reader"}, {"closed", true}});
    CHECK(h.nextText() == "reader closed: true");
}

TEST_CASE("reader preview: html needs no permission; a failed or cancelled download rejects the promise") {
    Host h(kScreen, {});
    const uint64_t s = h.host->openScreen("p", "main");
    h.nextText();
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "page"}});
    auto open = h.next(Kind::ReaderOpen);
    REQUIRE(open);
    CHECK(open->value["html"] == "<p>An article</p>");
    CHECK(open->value["baseUrl"] == "https://example.com/a");
    CHECK(h.nextText() == "asked");
    h.host->answerReader(open->requestId, "Cancelled");
    CHECK(h.nextText() == "rejected: Cancelled");
}

TEST_CASE("reader preview: from a zk.on handler it throws") {
    Host h(kScreen, {"net:arxiv.org"});
    const uint64_t s = h.host->openScreen("p", "main");
    h.nextText();
    h.host->dispatch(PluginEvent{"syncFinished", json::object(), false});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    h.host->screenEvent(s, {{"kind", "noop"}});
    CHECK(h.nextText().find("handler: Error: zk.reader.open: only in answer to the user") == 0);
}

// ---- Type sniffing ----------------------------------------------------------------------------

TEST_CASE("reader preview: the type comes from Content-Type, then the first bytes; HTML is told apart") {
    using T = PreviewType;
    CHECK(sniffPreviewType("application/pdf", "%PDF-1.7\n") == T::Pdf);
    CHECK(sniffPreviewType("application/pdf; charset=binary", "%PDF-1.4") == T::Pdf);
    CHECK(sniffPreviewType("application/epub+zip", std::string("PK\3\4", 4)) == T::Epub);
    CHECK(sniffPreviewType("text/html; charset=utf-8", "<!DOCTYPE html><html>") == T::Html);
    CHECK(sniffPreviewType("application/xhtml+xml", "<?xml version=\"1.0\"?><html>") == T::Html);
    // A server that doesn't say: the bytes decide.
    CHECK(sniffPreviewType("application/octet-stream", "%PDF-1.5") == T::Pdf);
    CHECK(sniffPreviewType("", "%PDF-1.5") == T::Pdf);
    std::string epub("PK\3\4", 4);
    epub += std::string(26, '\0') + "mimetypeapplication/epub+zip";
    CHECK(sniffPreviewType("application/octet-stream", epub) == T::Epub);
    CHECK(sniffPreviewType("binary/octet-stream", "\xEF\xBB\xBF  \n<!doctype html>") == T::Html);
    CHECK(sniffPreviewType("", "<HTML><body>") == T::Html);
    // A "PDF" link that answers with a page (a paywall, a landing page) opens as html.
    CHECK(sniffPreviewType("application/pdf", "<!DOCTYPE html><title>Sign in</title>") == T::Html);
    CHECK(sniffPreviewType("text/plain", "just words") == T::Unknown);
    CHECK(sniffPreviewType("application/zip", std::string("PK\3\4", 4) + "other") == T::Unknown);
}

// ---- The cache ---------------------------------------------------------------------------------

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static int n = 0;
        path = std::filesystem::temp_directory_path() / ("zk-preview-" + std::to_string(::getpid()) + "-" + std::to_string(++n));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string fileOf(const std::filesystem::path& dir, const std::string& name, size_t bytes) {
    const auto p = dir / name;
    std::ofstream(p, std::ios::binary) << std::string(bytes, 'x');
    return p.string();
}

}  // namespace

TEST_CASE("reader preview: the cache keeps the last 10 documents and at most 100 MB, the oldest going first") {
    TempDir t;
    int64_t now = 1000;
    PreviewCache cache((t.path / "preview").string(), [&] { return now; });
    for (int i = 0; i < 12; ++i) {
        ++now;
        std::string err;
        REQUIRE(cache.add(PreviewCache::keyForUrl("https://a/" + std::to_string(i)), fileOf(t.path, "d" + std::to_string(i), 1000),
                          PreviewType::Pdf, "Paper " + std::to_string(i), &err));
    }
    CHECK(cache.entries().size() == 10);
    CHECK_FALSE(cache.find(PreviewCache::keyForUrl("https://a/0")));
    CHECK_FALSE(cache.find(PreviewCache::keyForUrl("https://a/1")));
    // Reading one makes it the newest: it outlives the others.
    ++now;
    REQUIRE(cache.find(PreviewCache::keyForUrl("https://a/2")));
    for (int i = 12; i < 21; ++i) {
        ++now;
        REQUIRE(cache.add(PreviewCache::keyForUrl("https://a/" + std::to_string(i)), fileOf(t.path, "d" + std::to_string(i), 1000),
                          PreviewType::Pdf, "", nullptr));
    }
    CHECK(cache.find(PreviewCache::keyForUrl("https://a/2")));
    CHECK_FALSE(cache.find(PreviewCache::keyForUrl("https://a/3")));
    // Files of documents that went are gone from the disk; the kept ones' are there.
    size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(t.path / "preview"))
        if (e.path().filename() != "index.json") ++files;
    CHECK(files == 10);

    // 100 MB at most: two 45 MB documents and a third push out the oldest ones.
    ++now;
    REQUIRE(cache.add("big1", fileOf(t.path, "b1", 45u << 20), PreviewType::Pdf, "", nullptr));
    ++now;
    REQUIRE(cache.add("big2", fileOf(t.path, "b2", 45u << 20), PreviewType::Pdf, "", nullptr));
    ++now;
    REQUIRE(cache.add("big3", fileOf(t.path, "b3", 45u << 20), PreviewType::Pdf, "", nullptr));
    uint64_t total = 0;
    for (const auto& e : cache.entries()) total += e.bytes;
    CHECK(total <= (100u << 20));
    CHECK_FALSE(cache.find("big1"));
    CHECK(cache.find("big2"));
    CHECK(cache.find("big3"));
}

TEST_CASE("reader preview: the reading place is kept with the document, across a restart, until it leaves the cache") {
    TempDir t;
    int64_t now = 1;
    const std::string key = PreviewCache::keyForUrl("https://arxiv.org/pdf/2401.01234v2");
    {
        PreviewCache cache((t.path / "preview").string(), [&] { return now; });
        auto e = cache.add(key, fileOf(t.path, "d", 10), PreviewType::Pdf, "A paper", nullptr);
        REQUIRE(e);
        CHECK(e->position.empty());
        CHECK(std::filesystem::exists(e->file));
        CHECK(e->file.find(".pdf") != std::string::npos);
        cache.savePosition(key, "page:12");
    }
    PreviewCache again((t.path / "preview").string(), [&] { return now; });
    auto e = again.find(key);
    REQUIRE(e);
    CHECK(e->position == "page:12");
    CHECK(e->title == "A paper");
    CHECK(e->type == PreviewType::Pdf);
    // The same HTML is the same document; other HTML another.
    CHECK(PreviewCache::keyForHtml("<p>a</p>") == PreviewCache::keyForHtml("<p>a</p>"));
    CHECK(PreviewCache::keyForHtml("<p>a</p>") != PreviewCache::keyForHtml("<p>b</p>"));
    CHECK(PreviewCache::keyForUrl("https://x/a") != PreviewCache::keyForHtml("https://x/a"));
}

TEST_CASE("reader preview: html becomes a page the reader opens like a web snapshot; scripts go, data: images stay") {
    const std::string page = previewHtmlPage("<p>Text</p><script>alert(1)</script><SCRIPT src=x></SCRIPT><img src=\"data:image/png;base64,AAAA\">",
                                             "A <title> & more", "https://example.com/a/b");
    CHECK(page.find("<meta charset=\"utf-8\">") != std::string::npos);
    CHECK(page.find("<title>A &lt;title&gt; &amp; more</title>") != std::string::npos);
    CHECK(page.find("<base href=\"https://example.com/a/b\">") != std::string::npos);
    CHECK(page.find("<p>Text</p>") != std::string::npos);
    CHECK(page.find("alert") == std::string::npos);
    CHECK(page.find("SCRIPT") == std::string::npos);
    CHECK(page.find("data:image/png;base64,AAAA") != std::string::npos);
    // A whole document keeps its own head.
    const std::string whole = previewHtmlPage("<html><head><title>T</title></head><body><p>x</p></body></html>", "T", "");
    CHECK(whole.find("<p>x</p>") != std::string::npos);
    CHECK(whole.find("<meta charset=\"utf-8\">") != std::string::npos);
}

// ---- The reader in preview mode -----------------------------------------------------------------

namespace {

struct PreviewRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;
    std::vector<std::string> saved;
    int closed = 0;

    explicit PreviewRig(const std::string& position = "") {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 3);
        ItemRow row;
        row.title = "Gaussian random fields on manifolds";
        row.attachmentKey = "preview:u1";
        row.badge = "PDF";
        reader = std::make_unique<ReaderScreen>(row, "paper.pdf", DocType::Pdf, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
        }, 6);
        reader->setPreview({position, "https://arxiv.org/pdf/2401.01234v2", [this](const std::string& p) { saved.push_back(p); },
                            [this] { ++closed; }});
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(settle([&] { return reader->ready(); }));
    }
    bool settle(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

}  // namespace

TEST_CASE("reader preview mode: nothing is marked or saved to the library; the place goes to the cache") {
    PreviewRig rig;
    CHECK(rig.reader->preview());
    CHECK_FALSE(rig.reader->readOnlyLibrary());  // not a group's: its own reason
    const int libEvents = static_cast<int>(rig.nav.pluginSink.events.size());
    // The bookmark corner does nothing.
    rig.reader->onGesture(tapAt(ReaderScreen::bookmarkRect().center()), rig.nav);
    CHECK_FALSE(rig.reader->bookmarked());
    CHECK(rig.lib.bookmarks("preview:u1").empty());
    // Turning pages keeps the place in the preview, not in the library.
    rig.reader->turnTo(4);
    REQUIRE(rig.settle([&] { return rig.reader->page() == 4 && !rig.reader->turning(); }));
    REQUIRE_FALSE(rig.saved.empty());
    CHECK_FALSE(rig.saved.back().empty());
    CHECK(rig.lib.readingLocation("preview:u1").empty());
    CHECK(rig.lib.dirtyPositions().empty());
    // No plugin hears of a book opening.
    CHECK(static_cast<int>(rig.nav.pluginSink.events.size()) == libEvents);
    // Marking an area is refused, with a word on why.
    rig.reader->markArea(rig.nav);
    CHECK_FALSE(rig.reader->areaArmed());
    REQUIRE_FALSE(rig.nav.toasts.empty());
    CHECK(rig.nav.toasts.back().find("preview") != std::string::npos);
    // Closing tells whoever opened it, and syncs nothing.
    rig.reader->onClose(rig.nav);
    CHECK(rig.closed == 1);
    CHECK(rig.nav.syncFake.syncs == 0);

    // Opened again from the cache: the same place.
    PreviewRig again(rig.saved.back());
    CHECK(again.reader->page() == 4);
}

TEST_CASE("reader preview mode: a selection gets the Text bar; the menus offer Contents, Display and Light, and say Preview") {
    PreviewRig rig;
    const Point p{400, 400};
    rig.reader->onGesture(Gesture{GestureType::LongPress, p, p}, rig.nav);
    rig.reader->onGesture(Gesture{GestureType::LongPressUp, p, p}, rig.nav);
    REQUIRE(rig.settle([&] { return rig.reader->annotationBar() != nullptr; }));
    CHECK(rig.reader->annotationBar()->mode() == AnnotationBar::Mode::Text);
    CHECK(rig.nav.toasts.empty());  // as on a web page: no toast for selecting

    PreviewRig menus;
    menus.reader->onGesture(tapAt({ReaderScreen::pageArea().w / 2, ReaderScreen::pageArea().h / 2}), menus.nav);
    REQUIRE(menus.reader->menus());
    CHECK(menus.reader->menus()->buttonKinds() == std::vector<int>{0, 2, 3});
    CHECK(menus.reader->menus()->cell(1).empty());  // no Notes
    CHECK(menus.reader->menus()->cell(5).empty());  // no Mark area
    // Back closes the preview without a sync.
    menus.reader->onGesture(tapAt(ReaderMenus::backRect().center()), menus.nav);
    CHECK(menus.nav.pops == 1);
    CHECK(menus.nav.syncFake.syncs == 0);
}

// ---- Opening one: at once from the cache, else the download screen ----------------------------

TEST_CASE("reader preview: html opens in the reader at once; a url downloads on its screen, Cancel rejects; a cached one opens at once") {
    TempDir t;
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    int64_t now = 1;
    PreviewCache cache((t.path / "preview").string(), [&] { return now; });
    std::vector<std::string> answers;
    int downloads = 0;
    PreviewScreen::Deps deps;
    deps.cache = &cache;
    deps.makeReader = [](const PreviewEntry& e) {
        ItemRow row;
        row.title = e.title;
        row.attachmentKey = "preview:" + e.key;
        return std::make_unique<ReaderScreen>(row, e.file, e.type == PreviewType::Pdf ? DocType::Pdf : DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
        }, 6);
    };
    deps.answer = [&](const std::string& e) { answers.push_back(e); };
    std::atomic<bool> release{false};
    deps.download = [&](HttpRequest req) {
        ++downloads;
        if (req.onProgress) req.onProgress(512, 2048);
        while (!release && !*req.cancel) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (*req.cancel) throw std::runtime_error("zk.fetch(\"" + req.url + "\") was cancelled");
        std::ofstream(req.outFile, std::ios::binary) << "%PDF-1.7\n...";
        HttpResponse r;
        r.status = 200;
        r.headers["content-type"] = "application/octet-stream";
        r.url = req.url;
        return r;
    };

    PreviewScreen::show({"", "<p>An article</p>", "An article", ""}, deps, nav);
    REQUIRE(nav.pushed.size() == 1);
    CHECK(dynamic_cast<ReaderScreen*>(nav.pushed[0].get()));
    CHECK(dynamic_cast<ReaderScreen*>(nav.pushed[0].get())->preview());
    CHECK(answers == std::vector<std::string>{""});

    // A url: the download screen; Cancel rejects and goes back.
    PreviewScreen::show({"https://arxiv.org/pdf/2401.01234v2", "", "A paper", ""}, deps, nav);
    REQUIRE(nav.pushed.size() == 2);
    auto* screen = dynamic_cast<PreviewScreen*>(nav.pushed[1].get());
    REQUIRE(screen);
    nav.visible = screen;
    screen->onShow(nav);
    CHECK(screen->downloading());
    screen->onGesture(tapAt(PreviewScreen::cancelRect().center()), nav);
    CHECK(answers.back() == "Cancelled");
    CHECK(nav.pops == 1);

    // Again, to the end: the reader takes the screen's place, and the document is cached.
    PreviewScreen::show({"https://arxiv.org/pdf/2401.01234v2", "", "A paper", ""}, deps, nav);
    auto* second = dynamic_cast<PreviewScreen*>(nav.pushed.back().get());
    REQUIRE(second);
    nav.visible = second;
    second->onShow(nav);
    release = true;
    for (int i = 0; i < 400 && answers.size() < 3; ++i) {
        second->onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(answers.size() == 3);
    CHECK(answers.back() == "");
    CHECK(cache.find(PreviewCache::keyForUrl("https://arxiv.org/pdf/2401.01234v2"))->type == PreviewType::Pdf);
    const int before = downloads;
    const size_t pushed = nav.pushed.size();
    PreviewScreen::show({"https://arxiv.org/pdf/2401.01234v2", "", "A paper", ""}, deps, nav);
    CHECK(downloads == before);  // read before: no download
    CHECK(nav.pushed.size() == pushed + 1);
    CHECK(dynamic_cast<ReaderScreen*>(nav.pushed.back().get()));
    CHECK(answers.back() == "");
}

TEST_CASE("reader preview save mode: temporary highlights, Save menu button, and Save/Discard close prompt") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    addFontDir("assets/fonts-device");
    fillFixtureLibrary(lib, 3);

    auto makeSaveable = [&](ReaderScreen::PreviewSaveMode mode, ReaderScreen::PreviewCloseResult* got) {
        ItemRow row;
        row.title = "Fisk Organ in Lausanne";
        row.attachmentKey = "preview:g1";
        row.badge = "WEB";  // even HTML previews allow scratch highlights when saveMode != None
        auto r = std::make_unique<ReaderScreen>(
            row, "article.html", DocType::Epub,
            [] { return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); }); },
            6);
        r->setPreview({"", "", {}, {}, mode, [got](ReaderScreen::PreviewCloseResult res) { *got = std::move(res); }});
        nav.visible = r.get();
        r->onShow(nav);
        for (int i = 0; i < 400 && !r->ready(); ++i) {
            r->onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(r->ready());
        return r;
    };

    // Allow mode: selecting text shows the Create bar, highlighting stays in memory (not in library DB).
    ReaderScreen::PreviewCloseResult res;
    auto reader = makeSaveable(ReaderScreen::PreviewSaveMode::Allow, &res);
    CHECK(reader->previewCanSave());
    const Point p{400, 400};
    reader->onGesture(Gesture{GestureType::LongPress, p, p}, nav);
    reader->onGesture(Gesture{GestureType::LongPressUp, p, p}, nav);
    for (int i = 0; i < 400 && !reader->annotationBar(); ++i) {
        reader->onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(reader->annotationBar() != nullptr);
    CHECK(reader->annotationBar()->mode() == AnnotationBar::Mode::Create);
    // Tap Highlight (button 0).
    reader->onGesture(tapAt(reader->annotationBar()->buttonRect(0).center()), nav);
    REQUIRE(reader->previewAnnotations().size() == 1);
    CHECK(lib.annotationsFor("preview:g1").empty());  // nothing written to Zotero DB

    // Menus include Save (kind 7).
    reader->onGesture(tapAt({ReaderScreen::pageArea().w / 2, ReaderScreen::pageArea().h / 2}), nav);
    REQUIRE(reader->menus() != nullptr);
    CHECK(reader->menus()->buttonKinds() == std::vector<int>{0, 2, 3, 7});

    // Back with >=1 highlight opens the Save/Discard prompt instead of silently discarding.
    const int popsBefore = nav.pops;
    reader->onGesture(tapAt(ReaderMenus::backRect().center()), nav);
    CHECK(nav.pops == popsBefore);
    REQUIRE(reader->previewClosePrompt() != nullptr);
    // Tap Save on the prompt.
    reader->onGesture(tapAt(reader->previewClosePrompt()->yesRect().center()), nav);
    CHECK(nav.pops == popsBefore + 1);
    reader->onClose(nav);
    CHECK(res.action == "save");
    REQUIRE(res.annotations.size() == 1);
    CHECK_FALSE(res.annotations[0].text.empty());

    // Prompt mode: Back asks Save/Discard even with zero highlights; tapping Discard sends "discard".
    ReaderScreen::PreviewCloseResult res2;
    auto reader2 = makeSaveable(ReaderScreen::PreviewSaveMode::Prompt, &res2);
    reader2->onGesture(tapAt({ReaderScreen::pageArea().w / 2, ReaderScreen::pageArea().h / 2}), nav);
    REQUIRE(reader2->menus() != nullptr);
    const int pops2 = nav.pops;
    reader2->onGesture(tapAt(ReaderMenus::backRect().center()), nav);
    CHECK(nav.pops == pops2);
    REQUIRE(reader2->previewClosePrompt() != nullptr);
    reader2->onGesture(tapAt(reader2->previewClosePrompt()->noRect().center()), nav);
    CHECK(nav.pops == pops2 + 1);
    reader2->onClose(nav);
    CHECK(res2.action == "discard");
    CHECK(res2.annotations.empty());
}
