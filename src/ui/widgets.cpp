#include "ui/widgets.h"

#include <algorithm>

#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace zk {

using namespace theme;

namespace {

const TextStyle kChipStyle{kSans, 7, 500};
constexpr int kChipPad = 34, kChipGap = 18;

std::string thousandsOf(int64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

std::string metaOf(const ItemRow& r) {
    if (r.progress < 0) return "new";
    if (r.progress <= 0) return "opened";
    return std::to_string(static_cast<int>(r.progress * 100 + 0.5)) + "%";
}

}  // namespace

void drawItemRow(cairo_t* cr, Rect row, const ItemRow& item, const std::string& status) {
    fillRect(cr, row, 255);
    const int badgeW = 74;
    if (!item.badge.empty()) typeBadge(cr, {row.x, row.y + 26, badgeW, 92}, item.badge);
    else hRule(cr, row.y + 72, row.x + 20, row.x + badgeW - 20, kRule);
    const int tx = row.x + badgeW + 38, metaW = 150, tw = row.x + row.w - tx;
    const int bottom = row.y + row.h - 12;
    const TextStyle titleStyle{kSerif, 10, 700};
    const TextStyle bylineStyle{kSans, 7.5, 400, false, kSecondary};
    const TextStyle metaStyle{kSans, 6.5, 400, false, kTertiary};
    const TextStyle statusStyle{kSans, 7, 500, false, kInk};
    const std::string title = item.title.empty() ? "Untitled" : item.title;
    const std::string line2 = status.empty() ? item.byline : status;
    const TextStyle& line2Style = status.empty() ? bylineStyle : statusStyle;
    const int line2W = status.empty() ? tw - metaW - 12 : tw;
    const int line2H = measureText(line2.empty() ? " " : line2, line2Style, line2W, 2).height;
    const int titleY = row.y + 16;
    TextResult tr = drawText(cr, title, titleStyle, {tx, titleY, tw, bottom - line2H - 6 - titleY}, 2);
    const int y2 = titleY + tr.height + 6;
    drawText(cr, line2, line2Style, {tx, y2, line2W, bottom - y2}, status.empty() ? 1 : 2);
    if (status.empty()) {
        const std::string meta = metaOf(item);
        drawText(cr, meta, metaStyle, {row.x + row.w - metaW, y2, metaW, 50}, 1, Align::Right);
    }
    hRule(cr, row.y + row.h - 2, row.x, row.x + row.w);
}

Rect tabRect(Rect bar, int index, int count) {
    const int x0 = bar.x + bar.w * index / count, x1 = bar.x + bar.w * (index + 1) / count;
    return {x0, bar.y, x1 - x0, bar.h};
}

void drawTabBar(cairo_t* cr, Rect bar, const std::vector<std::string>& tabs, int active) {
    fillRect(cr, bar, 255);
    hRule(cr, bar.y, bar.x, bar.x + bar.w, 190);
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        Rect r = tabRect(bar, i, static_cast<int>(tabs.size()));
        const bool on = i == active;
        centredText(cr, tabs[i], {kSans, 8, on ? 700 : 400, false, on ? kInk : kSecondary}, r);
        if (on) fillRect(cr, {r.x + r.w / 2 - 40, bar.y + 2, 80, 6}, kInk);
    }
}

std::vector<Rect> chipRects(Rect area, const std::vector<std::string>& labels) {
    std::vector<Rect> out;
    int x = area.x;
    for (const auto& l : labels) {
        const int w = measureText(l, kChipStyle, area.w, 1).width + 2 * kChipPad;
        if (x + w > area.x + area.w) break;
        out.push_back({x, area.y, w, area.h});
        x += w + kChipGap;
    }
    return out;
}

void drawChips(cairo_t* cr, Rect area, const std::vector<std::string>& labels, int active) {
    fillRect(cr, area, 255);
    auto rects = chipRects(area, labels);
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect r = rects[i];
        const bool on = static_cast<int>(i) == active;
        gray(cr, on ? kInk : 170);
        roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, r.h / 2.0);
        if (on) {
            cairo_fill(cr);
        } else {
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
        }
        TextStyle st = kChipStyle;
        st.gray = on ? 255 : kInk;
        centredText(cr, labels[i], st, r);
    }
}

void drawBanner(cairo_t* cr, Rect r, const UserError& e) {
    fillRect(cr, r, 235);
    fillRect(cr, {r.x, r.y, 8, r.h}, kInk);
    const int x = r.x + kMargin, w = r.w - 2 * kMargin;
    TextResult m = drawText(cr, e.message, {kSans, 8, 500}, {x, r.y + 18, w, r.h - 60}, 2);
    if (!e.details.empty())
        drawText(cr, "Details: " + e.details, {kSans, 6, 400, false, kSecondary}, {x, r.y + 18 + m.height + 8, w, 40}, 1);
}

void drawToggle(cairo_t* cr, Rect r, bool on) {
    fillRect(cr, r, 255);
    const double h = std::min(r.h, 64), w = std::min(r.w, 112);
    const double x = r.x + r.w - w, y = r.y + (r.h - h) / 2;
    gray(cr, on ? kInk : 170);
    roundedRect(cr, x, y, w, h, h / 2);
    if (on) {
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
    }
    const double knob = h - 14;
    gray(cr, on ? 255 : 170);
    cairo_arc(cr, on ? x + w - 7 - knob / 2 : x + 7 + knob / 2, y + h / 2, knob / 2, 0, 6.2832);
    cairo_fill(cr);
}

void drawButton(cairo_t* cr, Rect r, const std::string& label, bool primary) {
    fillRect(cr, r, 255);
    gray(cr, primary ? kInk : 120);
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 14);
    if (primary) {
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
    }
    centredText(cr, label, {kSans, 8.5, 700, false, static_cast<uint8_t>(primary ? 255 : kInk)}, r);
}

void drawPageIndicator(cairo_t* cr, Rect r, int page, int pages) {
    fillRect(cr, r, 255);
    if (pages <= 1) return;
    centredText(cr, std::to_string(page + 1) + " / " + std::to_string(pages), {kSans, 6.5, 400, false, kTertiary}, r);
}

Rect headerBackRect() { return {0, 0, 150, kHeaderH}; }

void drawHeader(cairo_t* cr, const std::string& title, bool back) {
    fillRect(cr, {0, 0, 1236, kHeaderH}, 255);
    int x = kMargin;
    if (back) {
        gray(cr, kInk);
        cairo_set_line_width(cr, 6);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, kMargin + 26, 52);
        cairo_line_to(cr, kMargin + 4, 74);
        cairo_line_to(cr, kMargin + 26, 96);
        cairo_stroke(cr);
        x = kMargin + 64;
    }
    drawText(cr, title, {kSans, back ? 13.0 : 17.0, 700}, {x, back ? 40 : 18, 1236 - x - kMargin, 100}, 1);
}

void drawCollectionRow(cairo_t* cr, Rect r, const std::string& name, int64_t items, bool hasChildren) {
    fillRect(cr, r, 255);
    // Folder glyph.
    gray(cr, kSecondary);
    cairo_set_line_width(cr, 4);
    const double fx = r.x + 6, fy = r.y + r.h / 2.0 - 26;
    cairo_move_to(cr, fx, fy + 8);
    cairo_line_to(cr, fx + 22, fy + 8);
    cairo_line_to(cr, fx + 30, fy + 16);
    cairo_line_to(cr, fx + 62, fy + 16);
    cairo_line_to(cr, fx + 62, fy + 52);
    cairo_line_to(cr, fx, fy + 52);
    cairo_close_path(cr);
    cairo_stroke(cr);
    const int tx = r.x + 112, countW = 160;
    centredText(cr, name.empty() ? "Untitled" : name, {kSans, 9.5, 500}, {tx, r.y, r.w - (tx - r.x) - countW - 40, r.h},
                Align::Left);
    centredText(cr, thousandsOf(items), {kSans, 7.5, 400, false, kTertiary}, {r.x + r.w - countW - 40, r.y, countW, r.h},
                Align::Right);
    if (hasChildren) centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {r.x + r.w - 30, r.y, 30, r.h}, Align::Right);
    hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
}

}  // namespace zk
