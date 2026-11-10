#include "ui/widgets.h"

#include "ui/item_icons.h"
#include "ui/metrics.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

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
    itemTypeIcon(cr, {row.x, row.y + 26, badgeW, 92}, item.itemType, item.badge);
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
    if (status.empty() && item.read) {  // "✓ Read" instead of the progress, the tick drawn (fonts may lack ✓)
        const TextStyle readStyle{kSans, 6.5, 700, false, kInk};
        const int textW = measureText("Read", readStyle, metaW, 1).width;
        const int right = row.x + row.w;
        drawText(cr, "Read", readStyle, {right - textW, y2, textW, 50}, 1, Align::Right);
        const double cx = right - textW - 30, cy = y2 + 21;
        cairo_save(cr);
        gray(cr, kInk);
        cairo_set_line_width(cr, 4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, cx - 11, cy);
        cairo_line_to(cr, cx - 3, cy + 8);
        cairo_line_to(cr, cx + 12, cy - 9);
        cairo_stroke(cr);
        cairo_restore(cr);
    } else if (status.empty()) {
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

void drawToast(cairo_t* cr, Rect r, const std::string& text) {
    gray(cr, 40);
    roundedRect(cr, r.x, r.y, r.w, r.h, r.h / 2.0);
    cairo_fill(cr);
    centredText(cr, text, {kSans, 8, 500, false, 255}, {r.x + 40, r.y, r.w - 80, r.h});
}

std::string noteSyncMark(const AnnotationRecord& a) {
    if (a.conflict) return "conflict — Zotero's version kept";
    if (a.localOnly) return "on the Kindle only";
    if (a.dirty) return "not synced yet";
    return {};
}

Rect noteRowButtonRect(Rect row, int i) {
    constexpr int w = 200, h = 64, gap = 16;
    return {row.x + row.w - 2 * w - gap + i * (w + gap), row.y + row.h - h - 14, w, h};
}

namespace {

// An ink annotation's strokes, fitted into `box` (Zotero's PDF points, y up). Height used, or 0.
int drawInkPreview(cairo_t* cr, Rect box, const std::string& position) {
    std::vector<std::vector<std::pair<double, double>>> paths;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    try {
        const auto pos = nlohmann::json::parse(position);
        for (const auto& xy : pos.value("paths", nlohmann::json::array())) {
            std::vector<std::pair<double, double>> path;
            for (size_t i = 0; i + 1 < xy.size(); i += 2) {
                const double x = xy[i].get<double>(), y = xy[i + 1].get<double>();
                path.push_back({x, y});
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
            if (!path.empty()) paths.push_back(std::move(path));
        }
    } catch (...) {
        return 0;
    }
    if (paths.empty()) return 0;
    const double w = std::max(1.0, x1 - x0), h = std::max(1.0, y1 - y0);
    // At most about the size on a page (1 pt ~ 4 px at 300 ppi): a dot stays a dot.
    const double s = std::min({box.w / w, box.h / h, 3.0});
    const int usedH = static_cast<int>(std::ceil(h * s));
    gray(cr, theme::kInk);
    cairo_set_line_width(cr, 3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (const auto& path : paths) {
        for (size_t i = 0; i < path.size(); ++i) {
            const double x = box.x + (path[i].first - x0) * s, y = box.y + (y1 - path[i].second) * s;  // y up -> down
            i ? cairo_line_to(cr, x, y) : cairo_move_to(cr, x, y);
        }
        if (path.size() == 1) cairo_rel_line_to(cr, 0.1, 0);
    }
    cairo_stroke(cr);
    return usedH;
}

}  // namespace

std::string noteRowLabel(const AnnotationRecord& a) {
    if (a.type != "image") return "";
    return a.pageLabel.empty() ? "Area" : "Area · p. " + a.pageLabel;
}

void drawNoteRow(cairo_t* cr, Rect r, const AnnotationRecord& a, bool conflictButtons) {
    fillRect(cr, r, 255);
    fillRect(cr, {r.x, r.y + 20, 8, r.h - 40}, a.type == "note" ? kTertiary : kSecondary);
    const std::string mark = noteSyncMark(a);
    const int tx = r.x + 34, tw = r.w - 34 - 120;
    const int bottom = r.y + r.h - (mark.empty() ? 14 : 70);  // room for the mark (and a conflict's buttons)
    int y = r.y + 18;
    if (!a.text.empty()) {
        const int lines = mark.empty() ? 3 : a.comment.empty() ? 2 : 1;  // a marked row keeps room for its comment
        TextResult t = drawText(cr, a.text, {kSerif, 8.5, 400}, {tx, y, tw, std::min(132, bottom - y)}, lines);
        y += t.height + 8;
    } else if (a.type == "ink") {  // handwriting: the strokes themselves, small
        const int boxH = std::min(mark.empty() ? 150 : 90, bottom - y - (a.comment.empty() ? 0 : 44));
        if (const int used = boxH > 20 ? drawInkPreview(cr, {tx, y, std::min(tw, 900), boxH}, a.position) : 0)
            y += used + 8;
        else
            y += drawText(cr, "Handwriting", {kSans, 8, 400, true, kSecondary}, {tx, y, tw, 50}, 1).height + 8;
    } else if (a.type == "image") {
        y += drawText(cr, noteRowLabel(a), {kSans, 8, 400, true, kSecondary}, {tx, y, tw, 50}, 1).height + 8;
    }
    if (!a.comment.empty() && bottom - y >= 36)
        drawText(cr, a.comment, {kSans, 7, 400, false, kSecondary}, {tx, y, tw, bottom - y}, mark.empty() ? 2 : 1);
    if (!a.pageLabel.empty())
        drawText(cr, "p. " + a.pageLabel, {kSans, 6.5, 400, false, kTertiary}, {r.x + r.w - 110, r.y + 20, 110, 40}, 1,
                 Align::Right);
    if (!mark.empty()) {
        const int markW = a.conflict && conflictButtons ? noteRowButtonRect(r, 0).x - tx - 20 : tw;
        drawText(cr, mark, {kSans, 6.5, a.conflict ? 700 : 500, false, kSecondary}, {tx, r.y + r.h - 64, markW, 44}, 1);
    }
    if (a.conflict && conflictButtons) {
        drawButton(cr, noteRowButtonRect(r, 0), "Re-apply", false);
        drawButton(cr, noteRowButtonRect(r, 1), "Discard", false);
    }
    hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
}

Rect headerBackRect() { return {0, 0, 150, kHeaderH}; }

void drawHeader(cairo_t* cr, const std::string& title, bool back, int rightReserve) {
    fillRect(cr, {0, 0, metrics().width, kHeaderH}, 255);
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
    drawText(cr, title, {kSans, back ? 13.0 : 17.0, 700},
             {x, back ? 40 : 18, metrics().width - x - kMargin - rightReserve, 100}, 1);
}

void drawCollectionRow(cairo_t* cr, Rect r, const std::string& name, int64_t items, bool hasChildren, bool pinned,
                       const std::string& note) {
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
    const int nameW = r.w - (tx - r.x) - countW - 40 - (pinned ? 60 : 0);
    if (note.empty()) {
        centredText(cr, name.empty() ? "Untitled" : name, {kSans, 9.5, 500}, {tx, r.y, nameW, r.h}, Align::Left);
    } else {
        centredText(cr, name.empty() ? "Untitled" : name, {kSans, 9.5, 500}, {tx, r.y + 8, nameW, r.h / 2}, Align::Left);
        centredText(cr, note, {kSans, 7, 400, false, kSecondary}, {tx, r.y + r.h / 2 - 4, nameW, r.h / 2 - 10}, Align::Left);
    }
    if (pinned) {  // a pushpin: round head, short needle, slanted
        const double px = r.x + r.w - countW - 70, py = r.y + r.h / 2.0;
        gray(cr, kInk);
        cairo_save(cr);
        cairo_translate(cr, px, py);
        cairo_rotate(cr, 0.6);
        cairo_rectangle(cr, -9, -20, 18, 16);
        cairo_fill(cr);
        cairo_rectangle(cr, -15, -6, 30, 6);
        cairo_fill(cr);
        cairo_set_line_width(cr, 3);
        cairo_move_to(cr, 0, 0);
        cairo_line_to(cr, 0, 18);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    centredText(cr, thousandsOf(items), {kSans, 7.5, 400, false, kTertiary}, {r.x + r.w - countW - 40, r.y, countW, r.h},
                Align::Right);
    if (hasChildren) centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {r.x + r.w - 30, r.y, 30, r.h}, Align::Right);
    hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
}

}  // namespace zk
