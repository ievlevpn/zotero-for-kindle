#include "screens/contents.h"

#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {
constexpr int kW = 1236, kH = 1547, kLineH = 110;
const Rect kArea{0, kHeaderH, kW, kH - kHeaderH};
int perPage() { return (kArea.h - kPageIndicatorH) / kLineH; }
}  // namespace

ContentsScreen::ContentsScreen(std::vector<TocEntry> toc, int currentPage, std::function<void(const TocEntry&)> pick)
    : toc_(std::move(toc)), current_(currentPage), pick_(std::move(pick)) {}

void ContentsScreen::onShow(Nav&) {
    pager_ = Pager(static_cast<int64_t>(toc_.size()), perPage());
    // Start on the page holding the current chapter.
    int currentIndex = 0;
    for (size_t i = 0; i < toc_.size(); ++i)
        if (toc_[i].page <= current_) currentIndex = static_cast<int>(i);
    while (pager_.offset() + pager_.perPage() <= currentIndex && pager_.next()) {
    }
}

std::vector<Rect> ContentsScreen::entryRects() const {
    std::vector<Rect> out;
    for (int64_t i = pager_.offset(); i < static_cast<int64_t>(toc_.size()) && i < pager_.offset() + pager_.perPage(); ++i)
        out.push_back({kMargin, kArea.y + static_cast<int>(i - pager_.offset()) * kLineH, kW - 2 * kMargin, kLineH});
    return out;
}

void ContentsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    drawHeader(cr, "Contents", true);
    if (toc_.empty()) {
        drawText(cr, "This book has no table of contents.", {kSerif, 10, 400, false, kSecondary},
                 {kMargin, 500, kW - 2 * kMargin, 80}, 1, Align::Center);
        return;
    }
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
        drawText(cr, std::to_string(t.page + 1), {kSans, 7, 400, false, kTertiary}, {rects[k].x + rects[k].w - 150, rects[k].y + 34, 150, 50},
                 1, Align::Right);
        hRule(cr, rects[k].y + rects[k].h - 2, rects[k].x, rects[k].x + rects[k].w);
    }
    drawPageIndicator(cr, {0, kArea.y + kArea.h - kPageIndicatorH, kW, kPageIndicatorH}, pager_.page(), pager_.pages());
}

void ContentsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage(kArea, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    auto rects = entryRects();
    for (size_t k = 0; k < rects.size(); ++k)
        if (rects[k].contains(g.pos)) {
            const TocEntry chosen = toc_[static_cast<size_t>(pager_.offset()) + k];
            nav.pop();  // back to the reader first, then go there
            if (pick_) pick_(chosen);
            return;
        }
}

}  // namespace zk
