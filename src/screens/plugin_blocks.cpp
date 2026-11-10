#include "screens/plugin_blocks.h"

#include <algorithm>
#include <numeric>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;
using K = Block::Kind;

namespace {

constexpr int kButtonH = 110, kInputH = 100, kChipH = 76, kChipGapX = 18, kChipGapY = 18, kChipPad = 34;
constexpr int kCardPad = 32, kDividerH = 30, kLabelH = 48, kEmptyDeckH = 300;
const TextStyle kChipStyle{kSans, 7, 500};
const TextStyle kCardTitle{kSerif, 10.5, 700};
const TextStyle kCardMeta{kSans, 7, 400, false, kSecondary};
const TextStyle kCardBody{kSerif, 8.5, 400};
const TextStyle kBadge{kSans, 6, 700};
const TextStyle kNotice{kSans, 7.5, 400, true, kSecondary};
const TextStyle kInputText{kSans, 9, 400};
const TextStyle kRowTitle{kSans, 9.5, 500};
const TextStyle kRowSub{kSans, 7, 400, false, kSecondary};
const TextStyle kRowMeta{kSans, 7.5, 400, false, kTertiary};
const TextStyle kEmpty{kSerif, 10, 400, false, kSecondary};

int gapPx(Block::Gap g) {
    switch (g) {
        case Block::Gap::None: return 0;
        case Block::Gap::Small: return 18;
        case Block::Gap::Medium: return 36;
        case Block::Gap::Large: return 60;
    }
    return 18;
}

int spacePx(Block::Gap g) { return g == Block::Gap::Small ? 18 : g == Block::Gap::Large ? 72 : 36; }

TextStyle textStyle(Block::Style s) {
    TextStyle st;
    switch (s) {
        case Block::Style::Title: st = {kSerif, 11, 700}; break;
        case Block::Style::Body: st = {kSerif, 9, 400}; break;
        case Block::Style::Meta: st = {kSans, 7, 400, false, kSecondary}; break;
        case Block::Style::Label: st = {kSans, 6.5, 700, false, kSecondary}; break;
    }
    st.markup = true;
    return st;
}

std::string markupOf(const Block& b) {
    std::string m;
    for (const auto& s : b.spans) {
        std::string t = markupEscape(s.text);
        if (s.italic) t = "<i>" + t + "</i>";
        if (s.bold) t = "<b>" + t + "</b>";
        m += t;
    }
    return m;
}

int textH(const std::string& s, const TextStyle& st, int w, int lines) {
    return measureText(s.empty() ? " " : s, st, std::max(1, w), lines).height;
}

int rowH(const Block& list) {
    for (const auto& r : list.rows)
        if (!r.subtitle.empty()) return 150;
    return 112;
}

int inputLineH() { return textH("Xg", kInputText, 1000, 1); }

// The chips of `b` in lines `w` wide: per chip its line and rect relative to the block's top.
struct ChipPlace {
    int line;
    Rect rect;
};
std::vector<ChipPlace> chipPlaces(const Block& b, int w) {
    std::vector<ChipPlace> out;
    int x = 0, line = 0;
    for (const auto& c : b.chips) {
        const int cw = std::min(w, measureText(c.title.empty() ? " " : c.title, kChipStyle, w, 1).width + 2 * kChipPad);
        if (x > 0 && x + cw > w) x = 0, ++line;
        out.push_back({line, {x, line * (kChipH + kChipGapY), cw, kChipH}});
        x += cw + kChipGapX;
    }
    return out;
}

struct CardHead {
    int titleH = 0, metaH = 0, badgeW = 0, head = 0;
};
CardHead cardHead(const Block& card, int w) {
    CardHead h;
    const int inner = w - 2 * kCardPad;
    if (!card.badge.empty()) h.badgeW = std::min(inner / 2, measureText(card.badge, kBadge, inner, 1).width + 36);
    h.titleH = textH(card.title, kCardTitle, inner - (h.badgeW ? h.badgeW + 20 : 0), 3);
    h.metaH = card.meta.empty() ? 0 : textH(card.meta, kCardMeta, inner, 1) + 6;
    h.head = kCardPad + h.titleH + h.metaH + (card.body.empty() ? 0 : 24);
    return h;
}

// A block's height as page units: a fixed head and tail around units that may be split over pages.
struct Units {
    int head = 0, tail = 0;
    std::vector<int> h;
    bool breakable() const { return !h.empty(); }
    int sum() const { return std::accumulate(h.begin(), h.end(), 0); }
    int natural() const { return head + sum() + tail; }
};

int naturalH(const Block& b, int w);

std::vector<int> diffs(const std::vector<int>& tops) {
    std::vector<int> h;
    for (size_t i = 0; i + 1 < tops.size(); ++i) h.push_back(tops[i + 1] - tops[i]);
    return h;
}

// The widths of a row's children.
std::vector<int> rowWidths(const Block& row, int w) {
    const int n = static_cast<int>(row.children.size());
    std::vector<int> ws(n, 0);
    if (!n) return ws;
    const int gap = gapPx(row.gap);
    int fixed = 0, flex = 0;
    for (int i = 0; i < n; ++i) {
        if (row.children[i].kind == K::Button) {
            ws[i] = std::min(w / n, std::max(200, measureText(row.children[i].title, {kSans, 8.5, 700}, w, 1).width + 100));
            fixed += ws[i];
        } else {
            ++flex;
        }
    }
    const int rest = std::max(0, w - fixed - gap * (n - 1));
    if (flex == 0) {  // only buttons: right-aligned at their widths
        return ws;
    }
    for (int i = 0, k = 0; i < n; ++i)
        if (row.children[i].kind != K::Button) ws[i] = rest / flex + (k++ < rest % flex ? 1 : 0);
    return ws;
}

Units unitsOf(const Block& b, int w) {
    Units u;
    switch (b.kind) {
        case K::Text: {
            const auto tops = lineTops(markupOf(b), textStyle(b.style), w);
            if (b.lines > 0) u.head = tops[std::min<size_t>(b.lines, tops.size() - 1)];
            else u.h = diffs(tops);
            break;
        }
        case K::Card:
        case K::Deck: {
            if (b.kind == K::Deck && b.children.empty()) {
                u.head = kEmptyDeckH;
                break;
            }
            const Block& card = b.kind == K::Deck ? b.children[0] : b;
            u.head = cardHead(card, w).head;
            u.tail = kCardPad;
            if (!card.body.empty()) u.h = diffs(lineTops(card.body, kCardBody, w - 2 * kCardPad));
            break;
        }
        case K::List:
            if (b.rows.empty()) u.head = b.empty.empty() ? 0 : 112;
            else u.h.assign(b.rows.size(), rowH(b));
            break;
        case K::Chips: {
            const auto places = chipPlaces(b, w);
            const int lines = places.empty() ? 0 : places.back().line + 1;
            for (int i = 0; i < lines; ++i) u.h.push_back(kChipH + (i + 1 < lines ? kChipGapY : 0));
            break;
        }
        default: u.head = naturalH(b, w);
    }
    return u;
}

int naturalH(const Block& b, int w) {
    switch (b.kind) {
        case K::Column: {
            int h = 0;
            for (size_t i = 0; i < b.children.size(); ++i) h += (i ? gapPx(b.gap) : 0) + unitsOf(b.children[i], w).natural();
            return h;
        }
        case K::Row: {
            const auto ws = rowWidths(b, w);
            int h = 0;
            for (size_t i = 0; i < b.children.size(); ++i) h = std::max(h, unitsOf(b.children[i], ws[i]).natural());
            return h;
        }
        case K::Search: return kInputH;
        case K::Field: {
            const int box = b.lines <= 1 ? kInputH : 48 + b.lines * inputLineH();
            return (b.label.empty() ? 0 : kLabelH) + box;
        }
        case K::Button:
        case K::Buttons: return kButtonH;
        case K::Divider: return kDividerH;
        case K::Space: return spacePx(b.gap);
        case K::Notice: return textH(b.text, kNotice, w, 3);
        default: return unitsOf(b, w).natural();
    }
}

// Splits units into pages of at most `budget` px (at least one unit a page).
std::vector<std::pair<int, int>> split(const std::vector<int>& h, int budget) {
    std::vector<std::pair<int, int>> pages;
    int first = 0, used = 0;
    for (int i = 0; i < static_cast<int>(h.size()); ++i) {
        if (i > first && used + h[i] > budget) {
            pages.push_back({first, i - first});
            first = i, used = 0;
        }
        used += h[i];
    }
    pages.push_back({first, static_cast<int>(h.size()) - first});
    return pages;
}

Laid place(const Block& b, Rect r, int first, int count);

std::vector<Laid> stack(const std::vector<Block>& kids, Rect r, int gap) {
    std::vector<Laid> out;
    int y = r.y;
    for (const auto& c : kids) {
        if (y >= r.y + r.h) break;
        const Units u = unitsOf(c, r.w);
        const int h = std::min(u.natural(), r.y + r.h - y);
        out.push_back(place(c, {r.x, y, r.w, h}, 0, static_cast<int>(u.h.size())));
        y += h + gap;
    }
    return out;
}

Laid place(const Block& b, Rect r, int first, int count) {
    Laid l;
    l.block = &b;
    l.rect = r;
    l.first = first;
    l.count = count;
    switch (b.kind) {
        case K::Column: l.children = stack(b.children, r, gapPx(b.gap)); break;
        case K::Row: {
            const auto ws = rowWidths(b, r.w);
            const int gap = gapPx(b.gap);
            int total = std::accumulate(ws.begin(), ws.end(), 0) + gap * (static_cast<int>(ws.size()) - 1);
            int x = r.x + std::max(0, r.w - total);  // a row of buttons only sits on the right
            for (size_t i = 0; i < b.children.size(); ++i) {
                const Units u = unitsOf(b.children[i], ws[i]);
                const int h = std::min(u.natural(), r.h);
                l.children.push_back(place(b.children[i], {x, r.y + (r.h - h) / 2, ws[i], h}, 0, static_cast<int>(u.h.size())));
                x += ws[i] + gap;
            }
            break;
        }
        case K::List:
            for (int i = first; i < first + count; ++i)
                l.parts.push_back({i, Rect{r.x, r.y + (i - first) * rowH(b), r.w, rowH(b)}.intersected(r)});
            break;
        case K::Chips: {
            const auto places = chipPlaces(b, r.w);
            const int top = first * (kChipH + kChipGapY);
            for (size_t i = 0; i < places.size(); ++i)
                if (places[i].line >= first && places[i].line < first + count) {
                    const Rect c = places[i].rect;
                    l.parts.push_back({static_cast<int>(i), Rect{r.x + c.x, r.y + c.y - top, c.w, c.h}.intersected(r)});
                }
            break;
        }
        case K::Buttons: {
            const int n = static_cast<int>(b.children.size()), gap = 24;
            for (int i = 0; i < n; ++i) {
                const int x0 = r.x + (r.w + gap) * i / n, x1 = r.x + (r.w + gap) * (i + 1) / n - gap;
                l.parts.push_back({i, {x0, r.y, x1 - x0, std::min(kButtonH, r.h)}});
            }
            break;
        }
        default: break;
    }
    return l;
}

// ── Drawing ──

void border(cairo_t* cr, Rect r, uint8_t g, double radius, double width = 3) {
    gray(cr, g);
    cairo_set_line_width(cr, width);
    roundedRect(cr, r.x + width / 2, r.y + width / 2, r.w - width, r.h - width, radius);
    cairo_stroke(cr);
}

void pill(cairo_t* cr, Rect r, const std::string& text, bool filled) {
    gray(cr, filled ? kInk : 170);
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, r.h / 2.0);
    if (filled) {
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
    }
    TextStyle st = kChipStyle;
    st.gray = filled ? 255 : kInk;
    centredText(cr, text, st, {r.x + 24, r.y, r.w - 48, r.h});
}

void drawCard(cairo_t* cr, const Block& card, Rect r, int first, int count, bool deck) {
    const Rect box = r;
    fillRect(cr, {box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 255);
    border(cr, box, deck ? kSecondary : kTertiary, 16, deck ? 4 : 3);  // a deck's card: a firmer edge
    const CardHead h = cardHead(card, box.w);
    const int x = box.x + kCardPad, w = box.w - 2 * kCardPad;
    int y = box.y + kCardPad;
    if (h.badgeW) {
        const Rect b{x + w - h.badgeW, y, h.badgeW, 50};
        fillRect(cr, {b.x, b.y, b.w, b.h}, 235);
        centredText(cr, card.badge, kBadge, {b.x + 18, b.y, b.w - 36, b.h});
    }
    drawText(cr, card.title, kCardTitle, {x, y, w - (h.badgeW ? h.badgeW + 20 : 0), h.titleH + 4}, 3);
    y += h.titleH;
    if (h.metaH) drawText(cr, card.meta, kCardMeta, {x, y + 6, w, h.metaH}, 1);
    if (!card.body.empty()) drawTextLines(cr, card.body, kCardBody, {x, box.y + h.head, w, box.h}, first, count);
}

void drawRow(cairo_t* cr, const Block& list, const ListRow& row, Rect r) {
    fillRect(cr, r, 255);
    if (row.selected) fillRect(cr, {r.x, r.y + 18, 8, r.h - 36}, kInk);
    const int x = r.x + 34, metaW = row.meta.empty() ? 0 : std::min(200, measureText(row.meta, kRowMeta, 200, 1).width);
    int badgeW = row.badge.empty() ? 0 : std::min(r.w / 3, measureText(row.badge, kBadge, r.w, 1).width + 36);
    const int right = r.x + r.w - (metaW ? metaW + 20 : 0) - (badgeW ? badgeW + 16 : 0);
    TextStyle title = kRowTitle;
    if (row.selected) title.weight = 700;
    const bool two = rowH(list) > 112;
    if (two) {
        centredText(cr, row.title, title, {x, r.y + 14, right - x, r.h / 2 - 4}, Align::Left);
        centredText(cr, row.subtitle, kRowSub, {x, r.y + r.h / 2 - 2, right - x, r.h / 2 - 20}, Align::Left);
    } else {
        centredText(cr, row.title, title, {x, r.y, right - x, r.h}, Align::Left);
    }
    if (metaW) centredText(cr, row.meta, kRowMeta, {r.x + r.w - metaW, r.y, metaW, r.h}, Align::Right);
    if (badgeW) {
        const Rect b{right + 16, r.y + (r.h - 50) / 2, badgeW, 50};
        fillRect(cr, b, 235);
        centredText(cr, row.badge, kBadge, {b.x + 18, b.y, b.w - 36, b.h});
    }
    hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
}

std::string shownText(const Block& b, const FocusState& f) {
    auto it = f.texts.find(b.id);
    return it != f.texts.end() ? it->second : b.value.value_or("");
}

void drawInput(cairo_t* cr, const Block& b, Rect r, const FocusState& f) {
    Rect box = r;
    if (b.kind == K::Field && !b.label.empty()) {
        drawText(cr, b.label, textStyle(Block::Style::Label), {r.x, r.y + 4, r.w, kLabelH - 8}, 1);
        box.y += kLabelH, box.h -= kLabelH;
    }
    const bool focused = f.focused == b.id;
    const std::string text = shownText(b, f);
    const bool empty = text.empty();
    std::string shown = empty ? (focused ? "|" : b.placeholder) : text + (focused ? "|" : "");
    const TextStyle st{kSans, 9, 400, false, empty && !focused ? kTertiary : kInk};
    if (b.kind == K::Search) {
        border(cr, box, kInk, box.h / 2.0);
        searchIcon(cr, box.x + 54, box.y + box.h / 2.0 - 4, kTertiary);
        centredText(cr, shown, st, {box.x + 96, box.y, box.w - 120, box.h}, Align::Left);
    } else if (b.lines <= 1) {
        border(cr, box, focused ? kInk : kTertiary, 14, focused ? 4 : 3);
        centredText(cr, shown, st, {box.x + 30, box.y, box.w - 60, box.h}, Align::Left);
    } else {
        border(cr, box, focused ? kInk : kTertiary, 14, focused ? 4 : 3);
        drawText(cr, shown, st, {box.x + 30, box.y + 24, box.w - 60, box.h - 48}, b.lines);
    }
}

void drawLaid(cairo_t* cr, const Laid& l, const FocusState& f) {
    const Block& b = *l.block;
    const Rect r = l.rect;
    if (r.empty()) return;
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    switch (b.kind) {
        case K::Column:
        case K::Row:
            for (const auto& c : l.children) drawLaid(cr, c, f);
            break;
        case K::Text: {
            const std::string m = markupOf(b);
            if (b.lines > 0) drawText(cr, m, textStyle(b.style), r, b.lines);
            else drawTextLines(cr, m, textStyle(b.style), r, l.first, l.count);
            break;
        }
        case K::Card: drawCard(cr, b, r, l.first, l.count, false); break;
        case K::Deck:
            if (!b.children.empty()) {
                drawCard(cr, b.children[0], r, l.first, l.count, true);
            } else {
                border(cr, r, 200, 16);
                drawText(cr, b.empty, kEmpty, {r.x + 40, r.y + r.h / 2 - 30, r.w - 80, r.h / 2}, 2, Align::Center);
            }
            break;
        case K::List:
            if (b.rows.empty()) centredText(cr, b.empty, kEmpty, r);
            for (const auto& p : l.parts) drawRow(cr, b, b.rows[p.index], p.rect);
            break;
        case K::Search:
        case K::Field: drawInput(cr, b, r, f); break;
        case K::Chips: {
            auto ov = f.chips.find(b.id);
            const auto& sel = ov != f.chips.end() ? ov->second : b.selected;
            for (const auto& p : l.parts) {
                const Chip& c = b.chips[p.index];
                const bool on = b.select != Block::Select::None && std::find(sel.begin(), sel.end(), c.id) != sel.end();
                pill(cr, p.rect, c.title, on);
            }
            break;
        }
        case K::Button: drawButton(cr, {r.x, r.y, r.w, std::min(r.h, kButtonH)}, b.title, b.primary); break;
        case K::Buttons:
            for (const auto& p : l.parts) drawButton(cr, p.rect, b.children[p.index].title, b.children[p.index].primary);
            break;
        case K::Divider: hRule(cr, r.y + r.h / 2 - 1, r.x, r.x + r.w, kRule); break;
        case K::Space: break;
        case K::Notice: drawText(cr, b.text, kNotice, r, 3); break;
    }
    cairo_restore(cr);
}

void hitIn(const Laid& l, Point p, std::optional<Hit>& hit) {
    if (!l.rect.contains(p) || hit) return;
    const Block& b = *l.block;
    for (const auto& c : l.children) hitIn(c, p, hit);
    if (hit) return;
    switch (b.kind) {
        case K::Button: hit = Hit{b.id, "", b.kind, l.rect}; break;
        case K::Search:
        case K::Field:
        case K::Deck: hit = Hit{b.id, "", b.kind, l.rect}; break;
        case K::Buttons:
        case K::List:
        case K::Chips:
            for (const auto& part : l.parts)
                if (part.rect.contains(p)) {
                    if (b.kind == K::Buttons) hit = Hit{b.children[part.index].id, "", K::Button, part.rect};
                    else if (b.kind == K::List) hit = Hit{b.id, b.rows[part.index].id, b.kind, part.rect};
                    else hit = Hit{b.id, b.chips[part.index].id, b.kind, part.rect};
                }
            break;
        default: break;
    }
}

}  // namespace

Rect blocksArea() {
    const Metrics& m = metrics();
    return {kMargin, kHeaderH + 12, m.width - 2 * kMargin, m.height - kHeaderH - 12 - 24};
}

Laid layoutTree(const Block& root, Rect area, int page) {
    std::vector<const Block*> kids;
    int gap = 0;
    if (root.kind == K::Column) {
        for (const auto& c : root.children) kids.push_back(&c);
        gap = gapPx(root.gap);
    } else {
        kids.push_back(&root);
    }
    const int n = static_cast<int>(kids.size());
    std::vector<Units> units;
    int total = 0;
    for (int i = 0; i < n; ++i) {
        units.push_back(unitsOf(*kids[i], area.w));
        total += (i ? gap : 0) + units[i].natural();
    }
    Laid out;
    out.block = &root;
    out.rect = area;

    if (total <= area.h) {  // it all fits
        int y = area.y;
        for (int i = 0; i < n; ++i) {
            out.children.push_back(place(*kids[i], {area.x, y, area.w, units[i].natural()}, 0, static_cast<int>(units[i].h.size())));
            y += units[i].natural() + gap;
        }
        return out;
    }

    const int avail = area.h - kPageIndicatorH;
    int fixed = gap * std::max(0, n - 1), minimum = 0;
    for (const auto& u : units) {
        fixed += u.breakable() ? u.head + u.tail : u.natural();
        if (u.breakable()) minimum += u.h[0];
    }

    if (fixed + minimum <= avail) {
        // Elastic: the blocks that page share what the others leave, smallest needs first.
        std::vector<int> budget(n, 0), order;
        for (int i = 0; i < n; ++i)
            if (units[i].breakable()) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return units[a].sum() < units[b].sum(); });
        int room = avail - fixed, left = static_cast<int>(order.size());
        for (int i : order) {
            budget[i] = std::min(units[i].sum(), room / left--);
            room -= budget[i];
        }
        std::vector<std::vector<std::pair<int, int>>> split_(n);
        for (int i : order) {
            split_[i] = split(units[i].h, budget[i]);
            out.pages = std::max(out.pages, static_cast<int>(split_[i].size()));
        }
        out.page = std::clamp(page, 0, out.pages - 1);
        int y = area.y;
        for (int i = 0; i < n; ++i) {
            const Units& u = units[i];
            int h = u.natural(), first = 0, count = static_cast<int>(u.h.size());
            if (u.breakable() && split_[i].size() > 1) {
                const auto& pg = split_[i][std::min<size_t>(out.page, split_[i].size() - 1)];
                first = pg.first, count = pg.second;
                h = u.head + budget[i] + u.tail;
            }
            out.children.push_back(place(*kids[i], {area.x, y, area.w, h}, first, count));
            y += h + gap;
        }
        return out;
    }

    // Flow: the screen pages as a whole; a block that pages is split between screen pages (its
    // head and tail on each part), a block taller than a page is cut to it.
    struct Seg {
        int kid, first, count, y, h;
    };
    std::vector<std::vector<Seg>> pages(1);
    int y = 0;
    const auto newPage = [&] {
        pages.emplace_back();
        y = 0;
    };
    for (int i = 0; i < n; ++i) {
        const Units& u = units[i];
        if (!u.breakable()) {
            const int h = std::min(u.natural(), avail);
            if (y > 0 && y + gap + h > avail) newPage();
            const int top = y ? y + gap : 0;
            pages.back().push_back({i, 0, 0, top, h});
            y = top + h;
            continue;
        }
        int at = 0;
        const int count = static_cast<int>(u.h.size());
        while (at < count) {
            const int top = y ? y + gap : 0;
            int room = avail - top - u.head - u.tail, k = 0, used = 0;
            while (at + k < count && used + u.h[at + k] <= room) used += u.h[at + k++];
            if (k == 0) {
                if (y > 0) {
                    newPage();
                    continue;
                }
                k = 1, used = std::min(u.h[at], std::max(0, room));
            }
            pages.back().push_back({i, at, k, top, std::min(avail, u.head + used + u.tail)});
            y = top + pages.back().back().h;
            at += k;
            if (at < count) newPage();
        }
    }
    if (pages.back().empty() && pages.size() > 1) pages.pop_back();
    out.pages = static_cast<int>(pages.size());
    out.page = std::clamp(page, 0, out.pages - 1);
    for (const Seg& s : pages[out.page])
        out.children.push_back(place(*kids[s.kid], {area.x, area.y + s.y, area.w, s.h}, s.first, s.count));
    return out;
}

Rect pageIndicatorRect(const Laid& root) {
    if (root.pages <= 1) return {};
    return {root.rect.x, root.rect.y + root.rect.h - kPageIndicatorH, root.rect.w, kPageIndicatorH};
}

void drawTree(cairo_t* cr, const Laid& root, const FocusState& f) {
    fillRect(cr, root.rect, 255);
    for (const auto& c : root.children) drawLaid(cr, c, f);
    if (root.pages > 1) drawPageIndicator(cr, pageIndicatorRect(root), root.page, root.pages);
}

std::optional<Hit> hitTest(const Laid& root, Point p) {
    std::optional<Hit> hit;
    for (const auto& c : root.children) hitIn(c, p, hit);
    return hit;
}

const Laid* findLaid(const Laid& l, const std::string& id) {
    if (l.block && !id.empty() && l.block->id == id && l.block != nullptr) return &l;
    for (const auto& c : l.children)
        if (const Laid* f = findLaid(c, id)) return f;
    return nullptr;
}

}  // namespace zk
