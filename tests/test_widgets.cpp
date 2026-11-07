#include <doctest/doctest.h>

#include "golden.h"
#include "ui/pager.h"
#include "ui/text.h"
#include "ui/text_field.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;

TEST_CASE("pager: pages, bounds and growth") {
    Pager p(41, 8);
    CHECK(p.pages() == 6);
    CHECK(p.page() == 0);
    for (int i = 0; i < 5; ++i) CHECK(p.next());
    CHECK_FALSE(p.next());
    CHECK(p.offset() == 40);
    CHECK(p.prev());
    CHECK(p.offset() == 32);
    Pager q(41, 8);
    q.next();
    q.setTotal(100);
    CHECK(q.offset() == 8);
    CHECK(q.pages() == 13);
    q.setTotal(3);  // shrank under us: clamp to the last page
    CHECK(q.offset() == 0);
    Pager empty(0, 8);
    CHECK(empty.pages() == 1);
    CHECK_FALSE(empty.next());
}

TEST_CASE("text field: typing, backspace over multi-byte characters, submit") {
    TextField f;
    for (const char* s : {"П", "р", "и", "в", "е", "т"}) f.apply({KeyEvent::Code::Text, s});
    CHECK(f.text() == "Привет");
    f.apply({KeyEvent::Code::Backspace, ""});
    CHECK(f.text() == "Приве");
    f.clear();
    f.apply({KeyEvent::Code::Text, "a🦉"});
    f.apply({KeyEvent::Code::Backspace, ""});
    CHECK(f.text() == "a");
    f.apply({KeyEvent::Code::Backspace, ""});
    f.apply({KeyEvent::Code::Backspace, ""});
    CHECK(f.text().empty());
    CHECK_FALSE(f.submitted());
    f.apply({KeyEvent::Code::Return, ""});
    CHECK(f.submitted());
    f.apply({KeyEvent::Code::Text, "x"});
    CHECK_FALSE(f.submitted());
}

TEST_CASE("tabs and chips: even split, no overlap, inside the rect") {
    Rect bar{0, 1417, 1236, kTabBarH};
    int right = 0;
    for (int i = 0; i < 4; ++i) {
        Rect r = tabRect(bar, i, 4);
        CHECK(r.x == right);
        right = r.x + r.w;
    }
    CHECK(right == 1236);
    Rect chipsArea{60, 160, 1116, 80};
    auto rects = chipRects(chipsArea, {"All", "EPUB", "PDF", "Unread", "economics", "pinn"});
    for (size_t i = 0; i < rects.size(); ++i) {
        CHECK(rects[i].x >= chipsArea.x);
        CHECK(rects[i].x + rects[i].w <= chipsArea.x + chipsArea.w);
        if (i) CHECK(rects[i].x >= rects[i - 1].x + rects[i - 1].w);
    }
}

TEST_CASE("widgets sheet matches the reviewed golden image") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) {
        MESSAGE("skipped: no device fonts");
        return;
    }
    addFontDir("assets/fonts-device");
    Canvas c(1236, 1547);
    cairo_t* cr = c.cr();
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    ItemRow row{"IT000001", "Physics-informed neural networks", "Raissi et al. · 2019", "PDF", "PD000001",
                AttachmentRecord::State::Remote, -1};
    drawItemRow(cr, {60, 20, 1116, kRowH}, row, "");
    row.badge = "EPUB";
    row.title = "The Structure of Scientific Revolutions";
    row.byline = "Kuhn · 1962";
    row.progress = 0.71;
    drawItemRow(cr, {60, 20 + kRowH, 1116, kRowH}, row, "");
    drawItemRow(cr, {60, 20 + 2 * kRowH, 1116, kRowH}, row, "Downloading 42%");
    drawItemRow(cr, {60, 20 + 3 * kRowH, 1116, kRowH}, row, "Tailscale isn't running. Start it from KUAL, then try again.");
    drawChips(cr, {60, 790, 1116, 80}, {"All", "EPUB", "PDF", "Unread", "economics"}, 1);
    drawBanner(cr, {0, 890, 1236, 150}, {ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "curl 6: Could not resolve host"});
    drawToggle(cr, {60, 1070, 120, 70}, true);
    drawToggle(cr, {220, 1070, 120, 70}, false);
    drawButton(cr, {400, 1060, 340, 90}, "Connect", true);
    drawButton(cr, {780, 1060, 340, 90}, "Cancel", false);
    drawPageIndicator(cr, {0, 1180, 1236, 60}, 2, 41);
    drawTabBar(cr, {0, 1547 - kTabBarH, 1236, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 1);
    test::checkGolden(c, "tests/golden/widgets.png");
}
