#include "screens/book_notes.h"

#include "screens/annotation_bar.h"

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

BookNotesScreen::~BookNotesScreen() = default;

void BookNotesScreen::onShow(Nav& nav) { reload(nav); }

void BookNotesScreen::reload(Nav& nav) {
    all_ = nav.library().annotationsFor(row_.attachmentKey);
    const int perPage = (kArea.h - kPageIndicatorH) / kNoteRowH;
    if (pager_.perPage() != perPage) pager_ = Pager(static_cast<int64_t>(all_.size()), perPage);
    else pager_.setTotal(static_cast<int64_t>(all_.size()));  // after an edit: stay on this page
}

bool BookNotesScreen::wantsKeyboard() const { return editor_ && editor_->wantsKeyboard(); }

void BookNotesScreen::closeEditor(Nav& nav) {
    if (!editor_) return;
    if (editor_->takeChanged()) nav.sync().pushSoon();
    editor_.reset();
    reload(nav);
    nav.damage(kArea, Waveform::Quality);
}

void BookNotesScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (editor_ && editor_->onKey(k, nav) == AnnotationEditor::Result::Closed) closeEditor(nav);
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
    if (editor_) editor_->draw(cr);
}

void BookNotesScreen::onGesture(const Gesture& g, Nav& nav) {
    if (editor_) {
        if (g.type == GestureType::Tap && editor_->onTap(g.pos, nav) == AnnotationEditor::Result::Open) {
            if (editor_->takeChanged()) {
                reload(nav);
                nav.damage(kArea, Waveform::Quality);
                nav.sync().pushSoon();
            }
            return;
        }
        closeEditor(nav);
        return;
    }
    if (g.type == GestureType::LongPress) {
        auto rects = rowRects();
        for (size_t k = 0; k < rects.size(); ++k)
            if (rects[k].contains(g.pos)) {
                editor_ = std::make_unique<AnnotationEditor>(all_[static_cast<size_t>(pager_.offset()) + k], rects[k]);
                nav.damage(editor_->rect(), Waveform::Fast);
                return;
            }
        return;
    }
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
