#include "screens/book_notes.h"

#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {
constexpr int kW = 1236, kH = 1547;
const Rect kArea{0, kHeaderH, kW, kH - kHeaderH};
}  // namespace

BookNotesScreen::BookNotesScreen(ItemRow row, std::function<void(const AnnotationRecord&)> pick)
    : row_(std::move(row)), pick_(std::move(pick)) {}

void BookNotesScreen::onShow(Nav& nav) {
    all_ = nav.library().annotationsFor(row_.attachmentKey);
    pager_ = Pager(static_cast<int64_t>(all_.size()), (kArea.h - kPageIndicatorH) / kNoteRowH);
}

std::vector<Rect> BookNotesScreen::rowRects() const {
    std::vector<Rect> out;
    for (int64_t i = pager_.offset(); i < static_cast<int64_t>(all_.size()) && i < pager_.offset() + pager_.perPage(); ++i)
        out.push_back({kMargin, kArea.y + static_cast<int>(i - pager_.offset()) * kNoteRowH, kW - 2 * kMargin, kNoteRowH});
    return out;
}

void BookNotesScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    drawHeader(cr, "Notes", true);
    if (all_.empty()) {
        drawText(cr, "No highlights in this book yet.", {kSerif, 10, 400, false, kSecondary}, {kMargin, 500, kW - 2 * kMargin, 80},
                 1, Align::Center);
        return;
    }
    auto rects = rowRects();
    for (size_t k = 0; k < rects.size(); ++k) drawNoteRow(cr, rects[k], all_[static_cast<size_t>(pager_.offset()) + k]);
    drawPageIndicator(cr, {0, kArea.y + kArea.h - kPageIndicatorH, kW, kPageIndicatorH}, pager_.page(), pager_.pages());
}

void BookNotesScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage(kArea, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    auto rects = rowRects();
    for (size_t k = 0; k < rects.size(); ++k)
        if (rects[k].contains(g.pos)) {
            const AnnotationRecord chosen = all_[static_cast<size_t>(pager_.offset()) + k];
            nav.pop();
            if (pick_) pick_(chosen);
            return;
        }
}

}  // namespace zk
