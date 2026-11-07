// Plan 5 Task 4: selecting text and hitting an annotation, through the engine interface (and
// the GuardedEngine every module wraps its engine in).

#include <doctest/doctest.h>

#include "docs/guarded_engine.h"
#include "fake_engine.h"

using namespace zk;
using zk::test::FakeEngine;

namespace {

GuardedEngine guarded() { return GuardedEngine(std::make_unique<FakeEngine>()); }

AnnotationRecord on(int page, const std::string& key) {
    AnnotationRecord a;
    a.key = key;
    a.pageLabel = std::to_string(page);
    a.color = "#ffd400";
    a.type = "highlight";
    return a;
}

bool covers(const std::vector<Rect>& rects, Point p) {
    for (const auto& r : rects)
        if (r.contains(p)) return true;
    return false;
}

}  // namespace

TEST_CASE("selection api: a drag selects whole words and its rects cover the drag") {
    GuardedEngine e = guarded();
    auto s = e.select(3, {130, 210}, {450, 270});
    REQUIRE(s);
    CHECK(covers(s->rects, {130, 210}));
    CHECK(covers(s->rects, {450, 270}));
    CHECK(covers(s->rects, {100, 210}));  // snapped to the start of the first word
    CHECK_FALSE(s->text.empty());
    CHECK_FALSE(s->position.empty());
    CHECK_FALSE(s->sortIndex.empty());
    CHECK(s->pageLabel == "3");
    CHECK(s->roundTrips);
}

TEST_CASE("selection api: dragging backwards selects the same text") {
    GuardedEngine e = guarded();
    auto forward = e.select(3, {130, 210}, {450, 270});
    auto backward = e.select(3, {450, 270}, {130, 210});
    REQUIRE(forward);
    REQUIRE(backward);
    CHECK(forward->text == backward->text);
    CHECK(forward->position == backward->position);
}

TEST_CASE("selection api: a press outside the text selects nothing") {
    GuardedEngine e = guarded();
    CHECK_FALSE(e.select(3, {10, 10}, {20, 20}));
}

TEST_CASE("selection api: annotationAt finds the topmost highlight under the finger") {
    GuardedEngine e = guarded();
    std::vector<AnnotationRecord> anns = {on(3, "UNDER001"), on(3, "ONTOP001"), on(4, "OTHERPG1")};
    CHECK(e.annotationAt(3, {300, 220}, anns) == std::optional<std::string>("ONTOP001"));
    CHECK_FALSE(e.annotationAt(3, {50, 50}, anns));
    CHECK_FALSE(e.annotationAt(5, {300, 220}, anns));
}

TEST_CASE("selection api: an engine that throws while selecting gives no selection") {
    auto fake = std::make_unique<FakeEngine>();
    fake->throwOnSelect = true;
    GuardedEngine e(std::move(fake));
    CHECK_FALSE(e.select(3, {130, 210}, {450, 270}));
}
