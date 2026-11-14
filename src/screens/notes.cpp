#include "screens/notes.h"

#include <algorithm>

#include "screens/annotation_bar.h"
#include "screens/book.h"
#include "screens/collections.h"
#include "screens/read_tag.h"
#include "screens/search.h"
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
constexpr int kGroupH = 70, kLineH = 60, kTagButtonW = 160, kClearW = 160;
Rect confirmBox() { return {kMargin, 560, W() - 2 * kMargin, 300}; }

}  // namespace

Rect NotesScreen::searchRect() { return CollectionsScreen::searchRect(); }
Rect NotesScreen::tagButtonRect() { return {searchRect().x - 20 - kTagButtonW, 22, kTagButtonW, 80}; }
Rect NotesScreen::filterLineRect() { return {kMargin, kHeaderH, W() - 2 * kMargin, kLineH}; }
Rect NotesScreen::clearRect() { return {W() - kMargin - kClearW, kHeaderH, kClearW, kLineH}; }

Rect NotesScreen::contentArea() const {
    const int bottom = bottom_ > 0 ? bottom_ : H() - kTabBarH;
    const int top = kHeaderH + (tag_.empty() ? 0 : kLineH);
    return {0, top, W(), bottom - top};
}

std::string NotesScreen::filterLine() const {
    const int64_t n = pager_.total();
    return "tag: " + tag_ + " · " + withThousands(n) + (n == 1 ? " note" : " notes");
}

void NotesScreen::setTag(const std::string& tag, Nav& nav) {
    tag_ = tag;
    jump_.reset();
    pager_ = Pager(0, 1);  // from the first page
    load(nav.library());
    nav.damage({0, kHeaderH, W(), H() - kHeaderH - (searching_ ? 0 : kTabBarH)}, Waveform::Quality);
}

// Notes per page: as many as fit if each comes from a different book (its own header): 4 on the PW5.
int NotesScreen::perPage() const { return std::max(1, (contentArea().h - kPageIndicatorH) / (kNoteRowH + kGroupH)); }

Rect NotesScreen::footerRect() const { return {0, contentArea().y + contentArea().h - kPageIndicatorH, W(), kPageIndicatorH}; }
Rect NotesScreen::sheetArea() const { return contentArea(); }

Rect NotesScreen::confirmRect(int i) {
    const int w = (confirmBox().w - 3 * 40) / 2;
    return {confirmBox().x + 40 + i * (w + 40), confirmBox().y + confirmBox().h - 130, w, 90};
}

void NotesScreen::load(Library& lib) {
    allOffline_ = lib.allAnnotationsOffline();
    const int64_t offset = pager_.offset();
    lines_.clear();
    if (searching_ && query_.empty()) {
        pager_ = Pager(0, perPage());
        return;
    }
    pager_ = Pager(lib.countAnnotations(query_, tag_), perPage());
    while (pager_.offset() < offset && pager_.next()) {
    }
    std::string current;
    for (auto& n : lib.recentAnnotations(static_cast<int>(pager_.offset()), perPage(), query_, tag_)) {
        if (n.itemKey != current) {
            current = n.itemKey;
            lines_.push_back({true, n.itemKey, n.itemTitle, {}, {}});
        }
        std::string author = annotationAuthor(lib, n.annotation);
        lines_.push_back({false, n.itemKey, n.itemTitle, std::move(n), std::move(author)});
    }
}

void NotesScreen::onShow(Nav& nav) {
    if (searching_) {
        nav.keyboard().show();
        const Rect kb = nav.keyboard().bounds();
        bottom_ = kb.empty() ? H() : kb.y;
    }
    jump_.reset();
    load(nav.library());
}

void NotesScreen::search(Nav& nav) {
    pending_ = false;
    query_ = field_.text();
    pager_ = Pager(0, 1);  // from the first page
    load(nav.library());
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
}

void NotesScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (!searching_) return;
    if (k.code == KeyEvent::Code::Escape) {
        nav.keyboard().hide();
        nav.pop();
        return;
    }
    field_.apply(k);
    if (k.code == KeyEvent::Code::Return) return search(nav);
    pending_ = true;
    lastKeyMs_ = nav.nowMs();
    nav.damage(SearchScreen::fieldRect(), Waveform::Fast);
}

void NotesScreen::onTick(Nav& nav) {
    if (pending_ && nav.nowMs() - lastKeyMs_ >= kPauseMs) search(nav);
    if (pending_) nav.wakeAt(lastKeyMs_ + kPauseMs);  // the search after the pause in typing
}

std::vector<Rect> NotesScreen::lineRects() const {
    std::vector<Rect> out;
    int y = contentArea().y;
    for (const auto& l : lines_) {
        const int h = l.header ? kGroupH : kNoteRowH;
        out.push_back({kMargin, y, W() - 2 * kMargin, h});
        y += h;
    }
    return out;
}

void NotesScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    if (searching_) {
        drawHeader(cr, "", true);
        const Rect f = SearchScreen::fieldRect();
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        roundedRect(cr, f.x + 1.5, f.y + 1.5, f.w - 3, f.h - 3, f.h / 2.0);
        cairo_stroke(cr);
        searchIcon(cr, f.x + 54, f.y + f.h / 2.0 - 4, kTertiary);
        const bool empty = field_.text().empty();
        centredText(cr, empty ? "Search your highlights…" : field_.text() + "|",
                    {kSans, 9, 400, false, empty ? kTertiary : kInk}, {f.x + 96, f.y, f.w - 120, f.h}, Align::Left);
    } else {
        drawHeader(cr, "Notes", false, searchRect().w + kTagButtonW + 40);
        const Rect s = searchRect();
        searchIcon(cr, s.x + s.w / 2.0, s.y + s.h / 2.0, kInk);
        drawButton(cr, tagButtonRect(), "Tag", !tag_.empty());
    }
    if (!tag_.empty()) {
        const Rect line = filterLineRect();
        centredText(cr, filterLine(), {kSans, 7.5, 400, false, kSecondary}, {line.x, line.y, line.w - kClearW - 20, line.h},
                    Align::Left);
        centredText(cr, "Clear", {kSans, 7.5, 700, false, kInk}, clearRect(), Align::Right);
    }
    if (lines_.empty()) {
        if (searching_ && query_.empty()) return;
        const std::string onDemand = "Only books you've opened on the Kindle are searched. To search all highlights, "
                                     "turn on Keep all highlights offline in Settings.";
        const std::string msg = searching_ ? "Nothing found for “" + query_ + "”." + (allOffline_ ? "" : "\n" + onDemand)
                                : !tag_.empty() ? "No highlights or notes with this tag."
                                : allOffline_ ? "No highlights yet."
                                              : "Highlights appear here for books you've opened on the Kindle. To see all "
                                                "of them, turn on Keep all highlights offline in Settings.";
        drawText(cr, msg, {kSerif, 10, 400, false, kSecondary}, {kMargin + 40, 420, W() - 2 * kMargin - 80, 300}, 5,
                 Align::Center);
        return;
    }
    auto rects = lineRects();
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].header)
            drawText(cr, lines_[i].itemTitle.empty() ? "Untitled" : lines_[i].itemTitle, {kSans, 7.5, 700, false, kSecondary},
                     {rects[i].x, rects[i].y + 22, rects[i].w, 44}, 1);
        else
            drawNoteRow(cr, rects[i], lines_[i].note.annotation, true, lines_[i].author);
    }
    if (searching_) drawPageIndicator(cr, footerRect(), pager_.page(), pager_.pages());
    else drawJumpIndicator(cr, footerRect(), pager_.page(), pager_.pages());
    if (jump_) jump_->draw(cr, sheetArea());
    if (!discardKey_.empty()) {
        roundedRect(cr, confirmBox().x + 1.5, confirmBox().y + 1.5, confirmBox().w - 3, confirmBox().h - 3, 18);
        gray(cr, 255);
        cairo_fill_preserve(cr);
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        drawText(cr, "Discard your Kindle change? Zotero's version stays.", {kSans, 9, 500},
                 {confirmBox().x + 40, confirmBox().y + 40, confirmBox().w - 80, 120}, 2, Align::Center);
        drawButton(cr, confirmRect(0), "Discard", true);
        drawButton(cr, confirmRect(1), "Cancel", false);
    }
}

void NotesScreen::onGesture(const Gesture& g, Nav& nav) {
    if (jump_) {
        Nav* n = &nav;
        return jumpSheetGesture(jump_, g, sheetArea(), nav, pager_.page(), pager_.pages(), [](size_t) { return 0; },
                                [this, n](int page) {
                                    if (pager_.goTo(page)) load(n->library());
                                    n->damage(contentArea(), Waveform::Quality);
                                });
    }
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        load(nav.library());
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (searching_ && SearchScreen::fieldRect().contains(g.pos)) {
        nav.keyboard().show();
        const Rect kb = nav.keyboard().bounds();
        bottom_ = kb.empty() ? H() : kb.y;
        load(nav.library());
        nav.redrawAll(Waveform::Quality);
        return;
    }
    if (searching_ && headerBackRect().contains(g.pos)) {
        nav.keyboard().hide();
        nav.pop();
        return;
    }
    if (!searching_ && searchRect().contains(g.pos)) {
        nav.push(std::make_unique<NotesScreen>(open_, true));
        return;
    }
    if (!searching_ && discardKey_.empty() && tagButtonRect().contains(g.pos)) {
        Nav* n = &nav;
        nav.push(std::make_unique<TagPickerScreen>([this, n](const std::string& tag) { setTag(tag, *n); },
                                                   annotationTagFilter("")));
        return;
    }
    if (!tag_.empty() && discardKey_.empty() && clearRect().contains(g.pos)) return setTag("", nav);
    if (!searching_ && discardKey_.empty() && footerRect().contains(g.pos)) {
        if (pager_.pages() > 1) {
            jump_.emplace(std::vector<JumpGroup>{}, pager_.page(), pager_.pages());
            nav.damage(sheetArea(), Waveform::Ui);
        }
        return;
    }
    if (!discardKey_.empty()) {
        if (confirmRect(0).contains(g.pos)) nav.library().discardConflict(discardKey_);
        if (!confirmBox().contains(g.pos) || confirmRect(0).contains(g.pos) || confirmRect(1).contains(g.pos)) {
            discardKey_.clear();
            load(nav.library());
            nav.damage(contentArea(), Waveform::Quality);
        }
        return;
    }
    auto rects = lineRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(g.pos)) continue;
        const AnnotationRecord& a = lines_[i].note.annotation;
        if (!lines_[i].header && a.conflict) {
            if (noteRowButtonRect(rects[i], 0).contains(g.pos)) {
                const std::string result = nav.library().reapplyConflict(a.key);
                if (!result.empty())  // an edit of an annotation still there, or the annotation made again
                    plugins::annotationEvent(nav.plugins(), nav.library(),
                                             result == a.conflictOf ? "highlightChanged" : "highlightCreated", result);
                nav.sync().pushSoon();
                load(nav.library());
                nav.damage(contentArea(), Waveform::Quality);
                return;
            }
            if (noteRowButtonRect(rects[i], 1).contains(g.pos)) {
                discardKey_ = a.key;
                nav.damage(confirmBox(), Waveform::Ui);
                return;
            }
        }
        // A highlight opens its own file; the item's title, as a row's tap does.
        auto row = lines_[i].header ? nav.library().rowFor(lines_[i].itemKey) : nav.library().rowFor(lines_[i].itemKey, a.parentKey);
        if (!row) return;
        if (searching_) nav.keyboard().hide();
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
    nav.damage(contentArea(), Waveform::Quality);
}

}  // namespace zk
