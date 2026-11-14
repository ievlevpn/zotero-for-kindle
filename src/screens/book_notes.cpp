#include "screens/book_notes.h"

#include "screens/annotation_bar.h"
#include "screens/read_tag.h"

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
constexpr int kLineH = 60, kTagButtonW = 160, kClearW = 160;
}  // namespace

Rect BookNotesScreen::contentArea() const {
    const int top = kHeaderH + (tag_.empty() ? 0 : kLineH);
    return {0, top, W(), H() - top};
}
Rect BookNotesScreen::tagButtonRect() { return {W() - kMargin - kTagButtonW, 22, kTagButtonW, 80}; }
Rect BookNotesScreen::clearRect() { return {W() - kMargin - kClearW, kHeaderH, kClearW, kLineH}; }

std::string BookNotesScreen::filterLine() const {
    const size_t n = all_.size();
    return "tag: " + tag_ + " · " + std::to_string(n) + (n == 1 ? " note" : " notes");
}

void BookNotesScreen::setTag(const std::string& tag, Nav& nav) {
    tag_ = tag;
    pager_ = Pager(0, 1);
    reload(nav);
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
}

BookNotesScreen::BookNotesScreen(ItemRow row, std::function<void(const AnnotationRecord&)> pick)
    : row_(std::move(row)), pick_(std::move(pick)) {}

BookNotesScreen::~BookNotesScreen() = default;

void BookNotesScreen::onShow(Nav& nav) { reload(nav); }

void BookNotesScreen::reload(Nav& nav) {
    all_ = nav.library().annotationsFor(row_.attachmentKey);
    authors_.clear();
    for (const auto& a : all_)
        if (auto who = annotationAuthor(nav.library(), a); !who.empty()) authors_[a.key] = who;
    if (!tag_.empty())
        all_.erase(std::remove_if(all_.begin(), all_.end(),
                                  [&](const AnnotationRecord& a) {
                                      return std::find(a.tags.begin(), a.tags.end(), tag_) == a.tags.end();
                                  }),
                   all_.end());
    const int perPage = (contentArea().h - kPageIndicatorH) / kNoteRowH;
    if (pager_.perPage() != perPage) pager_ = Pager(static_cast<int64_t>(all_.size()), perPage);
    else pager_.setTotal(static_cast<int64_t>(all_.size()));  // after an edit: stay on this page
}

bool BookNotesScreen::wantsKeyboard() const { return editor_ && editor_->wantsKeyboard(); }

void BookNotesScreen::closeEditor(Nav& nav) {
    if (!editor_) return;
    if (editor_->takeChanged()) nav.sync().pushSoon();
    editor_.reset();
    reload(nav);
    nav.damage(contentArea(), Waveform::Quality);
}

void BookNotesScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (editor_ && editor_->onKey(k, nav) == AnnotationEditor::Result::Closed) closeEditor(nav);
}

std::vector<Rect> BookNotesScreen::rowRects() const {
    std::vector<Rect> out;
    for (int64_t i = pager_.offset(); i < static_cast<int64_t>(all_.size()) && i < pager_.offset() + pager_.perPage(); ++i)
        out.push_back({kMargin, contentArea().y + static_cast<int>(i - pager_.offset()) * kNoteRowH, W() - 2 * kMargin, kNoteRowH});
    return out;
}

void BookNotesScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Notes", true, kTagButtonW + 20);
    drawButton(cr, tagButtonRect(), "Tag", !tag_.empty());
    if (!tag_.empty()) {
        centredText(cr, filterLine(), {kSans, 7.5, 400, false, kSecondary}, {kMargin, kHeaderH, W() - 2 * kMargin - kClearW - 20, kLineH},
                    Align::Left);
        centredText(cr, "Clear", {kSans, 7.5, 700, false, kInk}, clearRect(), Align::Right);
    }
    if (all_.empty()) {
        drawText(cr, tag_.empty() ? "No highlights in this book yet." : "No highlights or notes with this tag.", {kSerif, 10, 400, false, kSecondary}, {kMargin, 500, W() - 2 * kMargin, 80},
                 1, Align::Center);
        return;
    }
    auto rects = rowRects();
    for (size_t k = 0; k < rects.size(); ++k) {
        const AnnotationRecord& a = all_[static_cast<size_t>(pager_.offset()) + k];
        drawNoteRow(cr, rects[k], a, false, authors_.count(a.key) ? authors_.at(a.key) : "");
    }
    drawPageIndicator(cr, {0, contentArea().y + contentArea().h - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
    if (editor_) editor_->draw(cr);
}

void BookNotesScreen::onGesture(const Gesture& g, Nav& nav) {
    if (editor_) {
        if (g.type == GestureType::Tap && editor_->onTap(g.pos, nav) == AnnotationEditor::Result::Open) {
            if (editor_->takeChanged()) {
                reload(nav);
                nav.damage(contentArea(), Waveform::Quality);
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
                const AnnotationRecord& a = all_[static_cast<size_t>(pager_.offset()) + k];
                editor_ = std::make_unique<AnnotationEditor>(a, rects[k], recentChips(nav.library()),
                                                             annotationReadOnly(nav.library(), a));
                nav.damage(editor_->rect(), Waveform::Ui);
                return;
            }
        return;
    }
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (tagButtonRect().contains(g.pos)) {
        Nav* n = &nav;
        nav.push(std::make_unique<TagPickerScreen>([this, n](const std::string& tag) { setTag(tag, *n); },
                                                   annotationTagFilter(row_.attachmentKey, true)));
        return;
    }
    if (!tag_.empty() && clearRect().contains(g.pos)) return setTag("", nav);
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
