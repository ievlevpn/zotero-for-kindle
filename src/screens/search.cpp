#include "screens/search.h"

#include <algorithm>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }
constexpr int kGapH = 20;  // between the field (or the scope chip) and the results
constexpr int kScopeH = 90;   // the scope chip's row
constexpr int kRecentH = 104, kRecentLabelH = 70;
constexpr int kMaxResults = 200;
const TextStyle kChipText{kSans, 7.5, 600};

}  // namespace

SearchScreen::SearchScreen(std::function<void(const ItemRow&)> openItem, StatusFn status, std::string scopeKey,
                           std::string scopeName)
    : openItem_(std::move(openItem)), scopeKey_(std::move(scopeKey)), scopeName_(std::move(scopeName)) {
    list_.setStatusFn(std::move(status));
}

int SearchScreen::top() const { return kHeaderH + (scopeKey_.empty() ? 0 : kScopeH); }

Rect SearchScreen::scopeRect() const {
    if (scopeKey_.empty()) return {};
    const int w = std::min(measureText("In: " + scopeName_, kChipText, W(), 1).width + 120, W() - 2 * kMargin);
    return {kMargin, kHeaderH + 6, w, kScopeH - 16};
}

Rect SearchScreen::clearRect() const { return {W() - kMargin - 180, areaCache_.y, 180, kRecentLabelH}; }

std::vector<Rect> SearchScreen::recentRects() const {
    std::vector<Rect> out;
    if (!searched_.empty()) return out;
    for (size_t i = 0; i < recent_.size(); ++i) {
        const int y = areaCache_.y + kRecentLabelH + static_cast<int>(i) * kRecentH;
        if (y + kRecentH > areaCache_.y + areaCache_.h) break;
        out.push_back({kMargin, y, W() - 2 * kMargin, kRecentH});
    }
    return out;
}

void SearchScreen::onSyncEvent(const SyncEvent& e, Nav& nav) { list_.onDownloadEvent(e, areaCache_, nav); }

Rect SearchScreen::fieldRect() { return {kMargin + 70, 30, W() - 2 * kMargin - 70, 92}; }

Rect SearchScreen::resultsArea(Nav& nav) const {
    const Rect kb = nav.keyboard().bounds();
    const int y = top() + kGapH;
    const int bottom = kb.empty() ? H() : kb.y;
    return {0, y, W(), bottom - y};
}

std::string SearchScreen::emptyMessage() const {
    return searched_.empty() ? std::string{} : "Nothing found for “" + searched_ + "”";
}

void SearchScreen::layout(Nav& nav) {
    areaCache_ = resultsArea(nav);
    list_.setRowsPerPage(ItemList::rowsFitting(areaCache_));
}

void SearchScreen::onShow(Nav& nav) {
    nav.keyboard().show();
    recent_ = nav.library().recentSearches();
    layout(nav);
}

void SearchScreen::search(Nav& nav) {
    pending_ = false;
    const std::string q = field_.text();
    searched_ = q;
    layout(nav);
    const std::string in = scopeKey_;
    list_.setSource(
        [q, in](Library& lib, int64_t offset, int limit) { return lib.searchRows(q, static_cast<int>(offset), limit, in); },
        [q, in](Library& lib) { return static_cast<int64_t>(lib.searchRows(q, 0, kMaxResults, in).size()); },
        nav.library());
    if (q.empty()) recent_ = nav.library().recentSearches();
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
}

void SearchScreen::leave(Nav& nav) {
    nav.keyboard().hide();
    nav.pop();
}

void SearchScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (k.code == KeyEvent::Code::Escape) {
        leave(nav);
        return;
    }
    field_.apply(k);
    if (k.code == KeyEvent::Code::Return) {
        nav.library().rememberSearch(field_.text());
        search(nav);
        return;
    }
    pending_ = true;
    lastKeyMs_ = nav.nowMs();
    nav.damage(fieldRect(), Waveform::Fast);
}

void SearchScreen::onTick(Nav& nav) {
    if (pending_ && nav.nowMs() - lastKeyMs_ >= kPauseMs) search(nav);
}

void SearchScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "", true);
    const Rect f = fieldRect();
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, f.x + 1.5, f.y + 1.5, f.w - 3, f.h - 3, f.h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, f.x + 54, f.y + f.h / 2.0 - 4, kTertiary);
    const bool empty = field_.text().empty();
    const std::string placeholder = scopeKey_.empty() ? "Search your library…" : "Search in " + scopeName_ + "…";
    centredText(cr, empty ? placeholder : field_.text() + "|", {kSans, 9, 400, false, empty ? kTertiary : kInk},
                {f.x + 96, f.y, f.w - 120, f.h}, Align::Left);
    if (const Rect c = scopeRect(); !c.empty()) {  // "In: Economics ✕"
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        roundedRect(cr, c.x + 1.5, c.y + 1.5, c.w - 3, c.h - 3, c.h / 2.0);
        cairo_stroke(cr);
        centredText(cr, "In: " + scopeName_, kChipText, {c.x + 34, c.y, c.w - 110, c.h}, Align::Left);
        const double cx = c.x + c.w - 48, cy = c.y + c.h / 2.0;
        cairo_set_line_width(cr, 4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, cx - 11, cy - 11);
        cairo_line_to(cr, cx + 11, cy + 11);
        cairo_move_to(cr, cx + 11, cy - 11);
        cairo_line_to(cr, cx - 11, cy + 11);
        cairo_stroke(cr);
    }
    const auto recents = recentRects();
    if (!recents.empty()) {
        const Rect a = areaCache_;
        centredText(cr, "RECENT", {kSans, 6.5, 500, false, kTertiary}, {kMargin, a.y, 400, kRecentLabelH}, Align::Left);
        centredText(cr, "Clear", {kSans, 7.5, 500}, clearRect(), Align::Right);
        for (size_t i = 0; i < recents.size(); ++i) {
            const Rect r = recents[i];
            centredText(cr, recent_[i], {kSans, 9, 400}, {r.x + 60, r.y, r.w - 60, r.h}, Align::Left);
            gray(cr, kTertiary);  // a small clock: run it again
            cairo_set_line_width(cr, 3);
            cairo_arc(cr, r.x + 20, r.y + r.h / 2.0, 16, 0, 2 * 3.14159265);
            cairo_move_to(cr, r.x + 20, r.y + r.h / 2.0 - 9);
            cairo_line_to(cr, r.x + 20, r.y + r.h / 2.0);
            cairo_line_to(cr, r.x + 27, r.y + r.h / 2.0 + 5);
            cairo_stroke(cr);
            hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
        }
    }
    if (!searched_.empty()) {
        list_.draw(cr, areaCache_);
        if (list_.rows().empty())
            drawText(cr, emptyMessage(), {kSerif, 11, 400, false, kSecondary}, {kMargin, areaCache_.y + 120, W() - 2 * kMargin, 100},
                     2, Align::Center);
    }
}

void SearchScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos) && !fieldRect().contains(g.pos)) {
        leave(nav);
        return;
    }
    if (g.type == GestureType::Tap && fieldRect().contains(g.pos)) {
        nav.keyboard().show();
        nav.redrawAll(Waveform::Quality);
        return;
    }
    if (g.type == GestureType::Tap && scopeRect().contains(g.pos)) {  // ✕: the whole library
        scopeKey_.clear();
        scopeName_.clear();
        if (!searched_.empty()) search(nav);
        layout(nav);
        nav.damage({0, 0, W(), H()}, Waveform::Quality);
        return;
    }
    if (g.type == GestureType::Tap) {
        const auto recents = recentRects();
        if (!recents.empty() && clearRect().contains(g.pos)) {
            nav.library().clearRecentSearches();
            recent_.clear();
            nav.damage(areaCache_, Waveform::Quality);
            return;
        }
        for (size_t i = 0; i < recents.size(); ++i)
            if (recents[i].contains(g.pos)) {
                const std::string q = recent_[i];
                field_.setText(q);
                nav.library().rememberSearch(q);
                search(nav);
                nav.damage(fieldRect(), Waveform::Quality);
                return;
            }
    }
    list_.onGesture(g, areaCache_, nav, [&](const ItemRow& r) {
        nav.library().rememberSearch(searched_);
        nav.keyboard().hide();
        if (openItem_) openItem_(r);
    });
}

}  // namespace zk
