#include "screens/home_demo.h"

#include <cmath>

#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace zk {

namespace {

using namespace theme;

constexpr int kMargin = 60;
constexpr int kTabBarHeight = 130;
constexpr int kCardY = 292;
constexpr int kCoverW = 290, kCoverH = 400;
constexpr int kRowH = 186;
constexpr int kRowsY = kCardY + kCoverH + 80 + 56;

}  // namespace

std::vector<Rect> homeDemoRowRects(const Canvas& canvas, const HomeDemoData& data) {
    std::vector<Rect> rows;
    for (int i = 0, y = kRowsY; i < (int)data.recent.size() && y + kRowH <= canvas.height() - kTabBarHeight;
         ++i, y += kRowH)
        rows.push_back({kMargin, y, canvas.width() - 2 * kMargin, kRowH});
    return rows;
}

Rect homeDemoProgressRect(const Canvas& canvas) {
    int colX = kMargin + kCoverW + 48;
    return {colX, kCardY + kCoverH - 92, canvas.width() - kMargin - colX, 10};
}

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
    // Bottom-up: progress bar, chapter just above it, title fills what's left.
    Rect bar = homeDemoProgressRect(canvas);
    const TextStyle chapterStyle{kSans, 8, 400, false, kSecondary};
    int chapterH = measureText(d.continueChapter, chapterStyle, colW, 1).height;
    int chapterY = bar.y - 24 - chapterH;
    drawText(cr, d.continueTitle, {kSerif, 13, 700}, {colX, cardY + 58, colW, chapterY - 16 - (cardY + 58)}, 3);
    drawText(cr, d.continueChapter, chapterStyle, {colX, chapterY, colW, chapterH + 4}, 1);
    progressBar(cr, bar, d.continueProgress);
    drawText(cr, d.continueMeta, {kSans, 7, 400, false, kSecondary}, {colX, cardY + cover.h - 64, colW, 70}, 2);
    hRule(cr, cardY + cover.h + 44, 0, W);

    // Recent.
    int recentY = cardY + cover.h + 80;
    drawText(cr, "RECENT", {kSans, 7, 700, false, kSecondary}, {kMargin, recentY, contentW, 50}, 1);
    std::vector<Rect> rows = homeDemoRowRects(canvas, d);
    for (size_t i = 0; i < rows.size(); ++i) {
        const HomeDemoItem& item = d.recent[i];
        Rect row = rows[i];
        typeBadge(cr, {row.x, row.y + 26, 74, 92}, item.type);
        int tx = row.x + 74 + 38, tw = row.x + row.w - tx, bottom = row.y + row.h - 12;
        const TextStyle byline{kSans, 7.5, 400, false, kSecondary}, meta{kSans, 6.5, 400, false, kTertiary};
        int bylineH = measureText(item.byline, byline, tw, 1).height;
        int metaH = measureText(item.meta, meta, tw, 1).height;
        // Two-line titles get byline and status on one line; never past the row's divider.
        TextResult title = measureText(item.title, {kSerif, 10, 700}, tw, 2);
        bool compact = title.lines > 1;
        int infoH = compact ? bylineH : bylineH + 4 + metaH;
        int titleY = row.y + 16;
        TextResult tr = drawText(cr, item.title, {kSerif, 10, 700}, {tx, titleY, tw, bottom - infoH - 6 - titleY}, 2);
        int ty = titleY + tr.height + 6;
        if (compact) {
            drawText(cr, item.byline + " · " + item.meta, byline, {tx, ty, tw, bylineH + 4}, 1);
        } else {
            drawText(cr, item.byline, byline, {tx, ty, tw, bylineH + 4}, 1);
            drawText(cr, item.meta, meta, {tx, ty + bylineH + 4, tw, metaH + 4}, 1);
        }
        hRule(cr, row.y + row.h - 2, row.x, row.x + row.w);
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
