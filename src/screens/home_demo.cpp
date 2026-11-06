#include "screens/home_demo.h"

#include <cmath>

#include "ui/text.h"
#include "ui/theme.h"

namespace zk {

namespace {

using namespace theme;

constexpr int kMargin = 60;
constexpr int kTabBarHeight = 130;

void gray(cairo_t* cr, uint8_t g) { cairo_set_source_rgb(cr, g / 255.0, g / 255.0, g / 255.0); }

void fillRect(cairo_t* cr, Rect r, uint8_t g) {
    gray(cr, g);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
}

void roundedRect(cairo_t* cr, double x, double y, double w, double h, double radius) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, x + radius, y + h - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void hRule(cairo_t* cr, int y, int x0, int x1, uint8_t g = kRule) { fillRect(cr, {x0, y, x1 - x0, 2}, g); }

// Text vertically centred in `box`.
void centredText(cairo_t* cr, const std::string& s, const TextStyle& st, Rect box, Align a = Align::Center) {
    TextResult m = measureText(s, st, box.w, 1);
    drawText(cr, s, st, {box.x, box.y + (box.h - m.height) / 2, box.w, m.height + 4}, 1, a);
}

void syncIcon(cairo_t* cr, double cx, double cy, double r) {
    gray(cr, kInk);
    cairo_set_line_width(cr, 5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, r, 0.25 * M_PI, 1.75 * M_PI);
    cairo_stroke(cr);
    // Arrowhead at the arc's end.
    double ex = cx + r * std::cos(1.75 * M_PI), ey = cy + r * std::sin(1.75 * M_PI);
    cairo_move_to(cr, ex - 14, ey - 4);
    cairo_line_to(cr, ex + 3, ey - 3);
    cairo_line_to(cr, ex + 1, ey + 14);
    cairo_stroke(cr);
}

void searchIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    gray(cr, g);
    cairo_set_line_width(cr, 5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, 15, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx + 11, cy + 11);
    cairo_line_to(cr, cx + 24, cy + 24);
    cairo_stroke(cr);
}

// Typographic cover for items without one: title in Bookerly, authors in Ember.
void generatedCover(cairo_t* cr, Rect r, const std::string& title, const std::string& authors, bool dark) {
    fillRect(cr, r, dark ? 42 : 255);
    if (!dark) {
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_rectangle(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3);
        cairo_stroke(cr);
    }
    uint8_t ink = dark ? 255 : kInk, soft = dark ? 200 : kSecondary;
    int pad = 26;
    drawText(cr, title, {kSerif, 9, 700, false, ink}, {r.x + pad, r.y + pad + 6, r.w - 2 * pad, r.h / 2}, 5);
    hRule(cr, r.y + r.h - 112, r.x + pad, r.x + pad + 60, soft);
    drawText(cr, authors, {kSans, 6.5, 400, false, soft}, {r.x + pad, r.y + r.h - 92, r.w - 2 * pad, 80}, 2);
}

void typeBadge(cairo_t* cr, Rect r, const std::string& type) {
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 6);
    cairo_stroke(cr);
    centredText(cr, type, {kSans, 5.5, 700}, r);
}

void progressBar(cairo_t* cr, Rect r, double fraction) {
    gray(cr, kRule);
    roundedRect(cr, r.x, r.y, r.w, r.h, r.h / 2.0);
    cairo_fill(cr);
    gray(cr, kInk);
    roundedRect(cr, r.x, r.y, std::max(double(r.h), r.w * fraction), r.h, r.h / 2.0);
    cairo_fill(cr);
}

}  // namespace

Rect homeDemoTabRect(const Canvas& canvas, int index, int tabCount) {
    int w = canvas.width(), y = canvas.height() - kTabBarHeight;
    int x0 = w * index / tabCount, x1 = w * (index + 1) / tabCount;
    return {x0, y, x1 - x0, kTabBarHeight};
}

void drawHomeDemo(Canvas& canvas, const HomeDemoData& d) {
    cairo_t* cr = canvas.cr();
    const int W = canvas.width(), H = canvas.height();
    const int contentW = W - 2 * kMargin;
    fillRect(cr, {0, 0, W, H}, 255);

    // Header: title + sync.
    drawText(cr, "Zotero", {kSans, 17, 700}, {kMargin, 34, contentW, 110}, 1);
    syncIcon(cr, W - kMargin - 22, 82, 20);

    // Search field.
    Rect search{kMargin, 150, contentW, 92};
    gray(cr, 170);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, search.x + 1.5, search.y + 1.5, search.w - 3, search.h - 3, search.h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, search.x + 54, search.y + search.h / 2.0 - 4, kTertiary);
    centredText(cr, "Search your library…", {kSans, 9, 400, false, kTertiary},
                {search.x + 96, search.y, search.w - 120, search.h}, Align::Left);

    // Continue reading card.
    int cardY = 292;
    Rect cover{kMargin, cardY, 290, 400};
    generatedCover(cr, cover, d.continueTitle, d.continueAuthors, true);
    int colX = cover.x + cover.w + 48, colW = W - kMargin - colX;
    drawText(cr, "CONTINUE READING", {kSans, 7, 700, false, kSecondary}, {colX, cardY + 6, colW, 50}, 1);
    TextResult t = drawText(cr, d.continueTitle, {kSerif, 13, 700}, {colX, cardY + 58, colW, 220}, 3);
    int y = cardY + 58 + t.height + 14;
    drawText(cr, d.continueChapter, {kSans, 8, 400, false, kSecondary}, {colX, y, colW, 50}, 1);
    progressBar(cr, {colX, cardY + cover.h - 92, colW, 10}, d.continueProgress);
    drawText(cr, d.continueMeta, {kSans, 7, 400, false, kSecondary}, {colX, cardY + cover.h - 64, colW, 70}, 2);
    hRule(cr, cardY + cover.h + 44, 0, W);

    // Recent.
    int recentY = cardY + cover.h + 80;
    drawText(cr, "RECENT", {kSans, 7, 700, false, kSecondary}, {kMargin, recentY, contentW, 50}, 1);
    int rowY = recentY + 56, rowH = 176;
    for (const HomeDemoItem& item : d.recent) {
        if (rowY + rowH > H - kTabBarHeight) break;
        typeBadge(cr, {kMargin, rowY + 26, 74, 92}, item.type);
        int tx = kMargin + 74 + 38, tw = W - kMargin - tx;
        TextResult tr = drawText(cr, item.title, {kSerif, 10, 700}, {tx, rowY + 18, tw, 120}, 2);
        int ty = rowY + 18 + tr.height + 6;
        drawText(cr, item.byline, {kSans, 7.5, 400, false, kSecondary}, {tx, ty, tw, 40}, 1);
        drawText(cr, item.meta, {kSans, 6.5, 400, false, kTertiary}, {tx, ty + 40, tw, 40}, 1);
        hRule(cr, rowY + rowH - 2, kMargin, W - kMargin);
        rowY += rowH;
    }

    // Tab bar.
    fillRect(cr, {0, H - kTabBarHeight, W, kTabBarHeight}, 255);
    hRule(cr, H - kTabBarHeight, 0, W, 190);
    for (int i = 0; i < (int)d.tabs.size(); ++i) {
        Rect r = homeDemoTabRect(canvas, i, (int)d.tabs.size());
        bool on = i == d.activeTab;
        centredText(cr, d.tabs[i], {kSans, 8, on ? 700 : 400, false, on ? kInk : kSecondary}, r);
        if (on) fillRect(cr, {r.x + r.w / 2 - 40, H - kTabBarHeight + 2, 80, 6}, kInk);
    }
}

}  // namespace zk
