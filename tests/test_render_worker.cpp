#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Rig {
    FakeEngine* engine = nullptr;
    std::unique_ptr<RenderWorker> worker;
    explicit Rig(int delayMs = 80, bool failOpen = false) {
        worker = std::make_unique<RenderWorker>([this, delayMs, failOpen](DocType) {
            auto e = std::make_unique<FakeEngine>();
            e->delayMs = delayMs;
            e->failOpen = failOpen;
            engine = e.get();
            return std::unique_ptr<DocumentEngine>(std::move(e));
        });
    }
    // Waits for the next event of `kind` (others are kept in `seen`).
    std::optional<RenderEvent> waitFor(RenderEvent::Kind kind, int timeoutMs = 2000) {
        const auto t0 = Clock::now();
        while (msSince(t0) < timeoutMs) {
            RenderEvent e;
            while (worker->poll(e)) {
                seen.push_back(e);
                if (e.kind == kind) return e;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::nullopt;
    }
    std::vector<RenderEvent> seen;
};

}  // namespace

TEST_CASE("render worker: opens, shows a page, then pre-renders its neighbours") {
    Rig rig;
    rig.worker->open("book.epub", DocType::Epub, Layout{}, DocPosition{"pos:450"});  // page 5 at 9 pt
    auto opened = rig.waitFor(RenderEvent::Kind::Opened);
    REQUIRE(opened);
    CHECK(opened->pageCount == 100);
    CHECK(opened->page == 5);
    CHECK(opened->toc.size() == 10);
    const auto t0 = Clock::now();
    auto page = rig.waitFor(RenderEvent::Kind::Page);
    REQUIRE(page);
    CHECK(page->page == 5);
    CHECK(msSince(t0) < 200);
    REQUIRE(page->image);
    CHECK(page->image->gray[0] == 5);
    CHECK(page->chapter == "Chapter 1");
    CHECK(page->progress == doctest::Approx(5.0 / 99));
    CHECK(page->wordsLeftInChapter == 5 * 300);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // neighbours render
    const auto t1 = Clock::now();
    rig.worker->show(6);
    auto next = rig.waitFor(RenderEvent::Kind::Page);
    REQUIRE(next);
    CHECK(next->page == 6);
    CHECK(msSince(t1) < 20);  // already rendered
}

TEST_CASE("render worker: fast repeated turns post only the last page") {
    Rig rig;
    rig.worker->open("book.epub", DocType::Epub, Layout{}, DocPosition{"pos:0"});
    REQUIRE(rig.waitFor(RenderEvent::Kind::Page));
    rig.seen.clear();
    for (int p : {5, 6, 7, 8, 9}) {
        rig.worker->show(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::optional<RenderEvent> last;
    while (auto e = rig.waitFor(RenderEvent::Kind::Page, 600)) last = e;
    REQUIRE(last);
    CHECK(last->page == 9);
    int pagesPosted = 0;
    for (const auto& e : rig.seen) pagesPosted += e.kind == RenderEvent::Kind::Page;
    CHECK(pagesPosted <= 2);  // at most the one in flight, then 9
}

TEST_CASE("render worker: a layout change keeps the passage") {
    Rig rig(0);
    rig.worker->open("book.epub", DocType::Epub, Layout{}, DocPosition{"pos:3600"});  // page 40 at 9 pt
    auto page = rig.waitFor(RenderEvent::Kind::Page);
    REQUIRE(page);
    CHECK(page->page == 40);
    Layout bigger;
    bigger.sizePt = 12;
    rig.worker->setLayout(bigger, page->position);
    auto relaid = rig.waitFor(RenderEvent::Kind::Opened);
    REQUIRE(relaid);
    CHECK(relaid->page == 30);
    CHECK(relaid->pageCount == 75);
}

TEST_CASE("render worker: jumps to positions and annotations; search runs on the worker") {
    Rig rig(0);
    rig.worker->open("book.epub", DocType::Epub, Layout{}, DocPosition{"pos:0"});
    REQUIRE(rig.waitFor(RenderEvent::Kind::Page));
    rig.worker->showPosition(DocPosition{"pos:1800"});
    auto p = rig.waitFor(RenderEvent::Kind::Page);
    REQUIRE(p);
    CHECK(p->page == 20);
    AnnotationRecord a;
    a.key = "ANN1";
    a.pageLabel = "33";
    rig.worker->highlightsFor({a});
    rig.worker->showAnnotation(a);
    // New highlights may first repost the current page (20) with them; wait for the jump.
    do p = rig.waitFor(RenderEvent::Kind::Page);
    while (p && p->page != 33);
    REQUIRE(p);
    CHECK(p->page == 33);
    REQUIRE(p->highlights.size() == 1);
    CHECK(p->highlights[0].annotationKey == "ANN1");
    rig.worker->search("whale");
    auto hits = rig.waitFor(RenderEvent::Kind::SearchResults);
    REQUIRE(hits);
    CHECK_FALSE(hits->hits.empty());
}

TEST_CASE("render worker: an open failure is one Error event; destroying mid-render is quick") {
    Rig failing(0, true);
    failing.worker->open("bad.epub", DocType::Epub, Layout{}, DocPosition{});
    auto err = failing.waitFor(RenderEvent::Kind::Error);
    REQUIRE(err);
    CHECK(err->error.message == "This EPUB is damaged and can't be opened.");

    Rig slow(80);
    slow.worker->open("book.epub", DocType::Epub, Layout{}, DocPosition{"pos:0"});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto t0 = Clock::now();
    slow.worker.reset();
    CHECK(msSince(t0) < 200);
}
