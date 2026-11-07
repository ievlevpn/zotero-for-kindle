#include "screens/notes.h"

#include "screens/book.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236, kH = 1547;
constexpr int kGroupH = 70;
const Rect kArea{0, kHeaderH, kW, kH - kTabBarH - kHeaderH};
constexpr int kPerPage = 4;  // notes per page; group headers fit in the remaining space
const Rect kConfirm{kMargin, 560, kW - 2 * kMargin, 300};

}  // namespace

Rect NotesScreen::confirmRect(int i) {
    const int w = (kConfirm.w - 3 * 40) / 2;
    return {kConfirm.x + 40 + i * (w + 40), kConfirm.y + kConfirm.h - 130, w, 90};
}

void NotesScreen::load(Library& lib) {
    allOffline_ = lib.allAnnotationsOffline();
    const int64_t offset = pager_.offset();
    pager_ = Pager(lib.countAnnotations(), kPerPage);
    while (pager_.offset() < offset && pager_.next()) {
    }
    lines_.clear();
    std::string current;
    for (auto& n : lib.recentAnnotations(static_cast<int>(pager_.offset()), kPerPage)) {
        if (n.itemKey != current) {
            current = n.itemKey;
            lines_.push_back({true, n.itemKey, n.itemTitle, {}});
        }
        lines_.push_back({false, n.itemKey, n.itemTitle, std::move(n)});
    }
}

void NotesScreen::onShow(Nav& nav) { load(nav.library()); }

std::vector<Rect> NotesScreen::lineRects() const {
    std::vector<Rect> out;
    int y = kArea.y;
    for (const auto& l : lines_) {
        const int h = l.header ? kGroupH : kNoteRowH;
        out.push_back({kMargin, y, kW - 2 * kMargin, h});
        y += h;
    }
    return out;
}

void NotesScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    drawHeader(cr, "Notes", false);
    if (lines_.empty()) {
        const std::string msg = allOffline_ ? "No highlights yet."
                                            : "Highlights appear here for books you've opened on the Kindle. To see all "
                                              "of them, turn on Keep all highlights offline in Settings.";
        drawText(cr, msg, {kSerif, 10, 400, false, kSecondary}, {kMargin + 40, 420, kW - 2 * kMargin - 80, 300}, 5,
                 Align::Center);
        return;
    }
    auto rects = lineRects();
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].header)
            drawText(cr, lines_[i].itemTitle.empty() ? "Untitled" : lines_[i].itemTitle, {kSans, 7.5, 700, false, kSecondary},
                     {rects[i].x, rects[i].y + 22, rects[i].w, 44}, 1);
        else
            drawNoteRow(cr, rects[i], lines_[i].note.annotation, true);
    }
    drawPageIndicator(cr, {0, kArea.y + kArea.h - kPageIndicatorH, kW, kPageIndicatorH}, pager_.page(), pager_.pages());
    if (!discardKey_.empty()) {
        roundedRect(cr, kConfirm.x + 1.5, kConfirm.y + 1.5, kConfirm.w - 3, kConfirm.h - 3, 18);
        gray(cr, 255);
        cairo_fill_preserve(cr);
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        drawText(cr, "Discard your Kindle change? Zotero's version stays.", {kSans, 9, 500},
                 {kConfirm.x + 40, kConfirm.y + 40, kConfirm.w - 80, 120}, 2, Align::Center);
        drawButton(cr, confirmRect(0), "Discard", true);
        drawButton(cr, confirmRect(1), "Cancel", false);
    }
}

void NotesScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        load(nav.library());
        nav.damage(kArea, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (!discardKey_.empty()) {
        if (confirmRect(0).contains(g.pos)) nav.library().discardConflict(discardKey_);
        if (!kConfirm.contains(g.pos) || confirmRect(0).contains(g.pos) || confirmRect(1).contains(g.pos)) {
            discardKey_.clear();
            load(nav.library());
            nav.damage(kArea, Waveform::Quality);
        }
        return;
    }
    auto rects = lineRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(g.pos)) continue;
        const AnnotationRecord& a = lines_[i].note.annotation;
        if (!lines_[i].header && a.conflict) {
            if (noteRowButtonRect(rects[i], 0).contains(g.pos)) {
                nav.library().reapplyConflict(a.key);
                nav.sync().pushSoon();
                load(nav.library());
                nav.damage(kArea, Waveform::Quality);
                return;
            }
            if (noteRowButtonRect(rects[i], 1).contains(g.pos)) {
                discardKey_ = a.key;
                nav.damage(kConfirm, Waveform::Ui);
                return;
            }
        }
        auto row = nav.library().rowFor(lines_[i].itemKey);
        if (!row) return;
        if (!open_) return nav.push(std::make_unique<BookScreen>(*row));  // no reader (screenshots)
        open_(*row, lines_[i].header ? std::nullopt : std::optional<AnnotationRecord>(a));
        return;
    }
}

void NotesScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished && e.kind != SyncEvent::Kind::AnnotationsReady &&
        e.kind != SyncEvent::Kind::PushFinished)
        return;
    load(nav.library());
    nav.damage(kArea, Waveform::Quality);
}

}  // namespace zk
