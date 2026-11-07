#include <doctest/doctest.h>

#include "docs/document.h"
#include "fake_engine.h"

using namespace zk;

TEST_CASE("document: Zotero colours become gray shades") {
    CHECK(shadeOf("#ffd400") == 220);
    CHECK(shadeOf("#ff6666") == 170);
    CHECK(shadeOf("#5fb236") == 190);
    CHECK(shadeOf("#2ea8e5") == 180);
    CHECK(shadeOf("#a28ae5") == 160);
    CHECK(shadeOf("#e56eee") == 200);
    CHECK(shadeOf("#f19837") == 210);
    CHECK(shadeOf("#aaaaaa") == 200);
    CHECK(shadeOf("#FFD400") == 220);  // case doesn't matter
    CHECK(shadeOf("") == 200);
    CHECK(shadeOf("chartreuse") == 200);
}

TEST_CASE("document: the fake engine behaves like a paginated book") {
    test::FakeEngine e;
    CHECK(e.pageCount() == 100);
    PageImage p = e.render(7);
    CHECK(p.width == 1236);
    CHECK(p.height == 1487);
    CHECK(p.gray[0] == 7);
    for (int i : {0, 1, 57, 99}) CHECK(e.pageOf(e.positionOfPage(i)) == i);
    const DocPosition pos = e.positionOfPage(40);
    Layout bigger = e.layout;
    bigger.sizePt = 12;
    e.setLayout(bigger);
    CHECK(e.pageOf(pos) == 30);  // the same passage, on another page after re-pagination
}
