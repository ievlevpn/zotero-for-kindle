#include "screens/note_popup.h"

#include <algorithm>

#include "screens/screen.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {
constexpr int kPad = 40, kTextTop = 100, kGap = 30, kButtonH = 90, kBottom = 36, kButtonGap = 24, kNear = 24, kTagGap = 20;
const TextStyle kBody{kSerif, 9, 400};
const char* kLabels[] = {"Edit", "Delete", "Close"};

std::string bodyOf(const AnnotationRecord& a) { return a.comment.empty() ? " " : a.comment; }
}  // namespace

NotePopup::NotePopup(AnnotationRecord note, Rect icon, Rect area, std::vector<std::string> chips)
    : note_(std::move(note)), icon_(icon), area_(area), tags_(std::move(chips), note_.tags) {
    const int w = std::min(area.w - kMargin, 900);
    tops_ = lineTops(bodyOf(note_), kBody, w - 2 * kPad);
    const int chrome = kTextTop + kGap + TagRow::kH + kTagGap + kButtonH + kBottom;
    const int lineH = tops_.size() > 1 ? tops_[1] : 50;
    const int room = std::max(lineH, area.h * 11 / 20 - chrome);
    starts_.assign(1, 0);
    for (int i = 0; i + 1 < static_cast<int>(tops_.size()); ++i)
        if (tops_[i + 1] - tops_[starts_.back()] > room) starts_.push_back(i);
    const int h = chrome + std::min(tops_.back(), room);
    // Beside the note: centred under it, else over it, kept on the page area.
    const int x = std::clamp(icon.x + icon.w / 2 - w / 2, area.x + kMargin / 2, area.x + area.w - kMargin / 2 - w);
    int y = icon.y + icon.h + kNear;
    if (y + h > area.y + area.h - kMargin / 2) y = icon.y - kNear - h;
    y = std::clamp(y, area.y + kMargin / 2, std::max(area.y + kMargin / 2, area.y + area.h - kMargin / 2 - h));
    rect_ = {x, y, w, h};
    tags_.place({x + kPad, y + h - kBottom - kButtonH - kTagGap - TagRow::kH, w - 2 * kPad, TagRow::kH});
}

std::string NotePopup::title() const {
    const std::string l = noteRowLabel(note_);
    return l.empty() ? "Note" : l;
}

Rect NotePopup::buttonRect(int i) const {
    const int bw = (rect_.w - 2 * kPad - 2 * kButtonGap) / 3;
    return {rect_.x + kPad + i * (bw + kButtonGap), rect_.y + rect_.h - kBottom - kButtonH, bw, kButtonH};
}

void NotePopup::draw(cairo_t* cr) const {
    const Rect r = rect_;
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 18);
    gray(cr, 255);
    cairo_fill_preserve(cr);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    const int indicatorW = pages() > 1 ? 200 : 0;
    drawText(cr, title(), {kSans, 8, 500}, {r.x + kPad, r.y + 30, r.w - 2 * kPad - indicatorW, 50}, 1);
    if (pages() > 1) drawPageIndicator(cr, {r.x + r.w - kPad - indicatorW, r.y + 22, indicatorW, 64}, page_, pages());
    const int first = starts_[page_];
    const int end = page_ + 1 < pages() ? starts_[page_ + 1] : static_cast<int>(tops_.size()) - 1;
    if (note_.comment.empty())
        drawText(cr, "Empty note", {kSerif, 9, 400, true, kTertiary}, {r.x + kPad, r.y + kTextTop, r.w - 2 * kPad, 60}, 1);
    else
        drawTextLines(cr, note_.comment, kBody,
                      {r.x + kPad, r.y + kTextTop, r.w - 2 * kPad, r.h - kTextTop - kGap - TagRow::kH - kTagGap - kButtonH - kBottom}, first,
                      end - first);
    tags_.draw(cr);
    for (int i = 0; i < 3; ++i) drawButton(cr, buttonRect(i), kLabels[i], i == 0);
}

NotePopup::Action NotePopup::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Left || g.dir == Direction::Right)) {
        const int to = std::clamp(page_ + (g.dir == Direction::Left ? 1 : -1), 0, pages() - 1);
        if (to != page_) {
            page_ = to;
            nav.damage(rect_, Waveform::Ui);
        }
        return Action::None;
    }
    if (g.type != GestureType::Tap) return Action::None;
    if (const int t = tags_.hit(g.pos); t == TagRow::kAdd) return Action::AddTag;
    else if (t >= 0) {
        chip_ = t;
        return Action::Tag;
    }
    if (buttonRect(0).contains(g.pos)) return Action::Edit;
    if (buttonRect(1).contains(g.pos)) return Action::Delete;
    if (buttonRect(2).contains(g.pos) || !rect_.contains(g.pos)) return Action::Close;
    return Action::None;
}

}  // namespace zk
