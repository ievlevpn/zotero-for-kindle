#include "screens/contents.h"

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
constexpr int kLineH = 110, kTabsH = 110;
}  // namespace

Rect ContentsScreen::contentArea() const {
    const int top = kHeaderH + (tabs_ ? kTabsH : 0);
    return {0, top, W(), H() - top};
}
int ContentsScreen::perPage() const { return (contentArea().h - kPageIndicatorH) / kLineH; }
Rect ContentsScreen::tabRect(int i) { return {kMargin + i * 320, kHeaderH + 10, 300, 84}; }
Rect ContentsScreen::goToRect() { return {W() - kMargin - 240, 34, 240, 84}; }

void ContentsScreen::setBookmarks(std::vector<TocEntry> marks) {
    marks_ = std::move(marks);
    tabs_ = true;
}

void ContentsScreen::setPageNumbers(std::function<int(int)> f) { pageNumber_ = std::move(f); }

std::string ContentsScreen::pageNumberText(const TocEntry& t) const {
    return std::to_string(pageNumber_ ? pageNumber_(t.page) : t.page + 1);
}

void ContentsScreen::setGoTo(std::function<void(Nav&)> f) { goTo_ = std::move(f); }

ContentsScreen::ContentsScreen(std::vector<TocEntry> toc, int currentPage, std::function<void(const TocEntry&)> pick)
    : toc_(std::move(toc)), current_(currentPage), pick_(std::move(pick)) {}

void ContentsScreen::onShow(Nav&) {
    pager_ = Pager(static_cast<int64_t>(shown().size()), perPage());
    if (tab_ == 1) return;
    // Start on the page holding the current chapter.
    int currentIndex = 0;
    for (size_t i = 0; i < toc_.size(); ++i)
        if (toc_[i].page <= current_) currentIndex = static_cast<int>(i);
    while (pager_.offset() + pager_.perPage() <= currentIndex && pager_.next()) {
    }
}

std::vector<Rect> ContentsScreen::entryRects() const {
    std::vector<Rect> out;
    for (int64_t i = pager_.offset(); i < static_cast<int64_t>(shown().size()) && i < pager_.offset() + pager_.perPage(); ++i)
        out.push_back({kMargin, contentArea().y + static_cast<int>(i - pager_.offset()) * kLineH, W() - 2 * kMargin, kLineH});
    return out;
}

void ContentsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Contents", true);
    if (goTo_) drawButton(cr, goToRect(), "Go to…", false);
    if (tabs_) {
        drawButton(cr, tabRect(0), "Chapters", tab_ == 0);
        drawButton(cr, tabRect(1), "Bookmarks" + (marks_.empty() ? std::string() : " (" + std::to_string(marks_.size()) + ")"),
                   tab_ == 1);
    }
    if (shown().empty()) {
        drawText(cr, tab_ == 1 ? "No bookmarks yet. Tap a page's top-right corner to add one." : "This book has no table of contents.",
                 {kSerif, 10, 400, false, kSecondary}, {kMargin, 500, W() - 2 * kMargin, 160}, 2, Align::Center);
        return;
    }
    const std::vector<TocEntry>& toc_ = shown();
    auto rects = entryRects();
    int currentIndex = -1;
    for (size_t i = 0; i < toc_.size(); ++i)
        if (toc_[i].page <= current_) currentIndex = static_cast<int>(i);
    for (size_t k = 0; k < rects.size(); ++k) {
        const size_t i = static_cast<size_t>(pager_.offset()) + k;
        const TocEntry& t = toc_[i];
        const int indent = std::min(t.level, 4) * 50;
        const bool here = static_cast<int>(i) == currentIndex;
        drawText(cr, t.title.empty() ? "…" : t.title, {kSerif, 9, here ? 700 : 400},
                 {rects[k].x + indent, rects[k].y + 30, rects[k].w - indent - 160, 60}, 1);
        drawText(cr, pageNumberText(t), {kSans, 7, 400, false, kTertiary}, {rects[k].x + rects[k].w - 150, rects[k].y + 34, 150, 50},
                 1, Align::Right);
        hRule(cr, rects[k].y + rects[k].h - 2, rects[k].x, rects[k].x + rects[k].w);
    }
    drawPageIndicator(cr, {0, contentArea().y + contentArea().h - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void ContentsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (goTo_ && goToRect().contains(g.pos)) {
        goTo_(nav);
        return;
    }
    if (tabs_)
        for (int i = 0; i < 2; ++i)
            if (tabRect(i).contains(g.pos) && tab_ != i) {
                tab_ = i;
                pager_ = Pager(static_cast<int64_t>(shown().size()), perPage());
                nav.damage({0, 0, W(), H()}, Waveform::Ui);
                return;
            }
    auto rects = entryRects();
    for (size_t k = 0; k < rects.size(); ++k)
        if (rects[k].contains(g.pos)) {
            const TocEntry chosen = shown()[static_cast<size_t>(pager_.offset()) + k];
            nav.pop();  // back to the reader first, then go there
            if (pick_) pick_(chosen);
            return;
        }
}

}  // namespace zk
