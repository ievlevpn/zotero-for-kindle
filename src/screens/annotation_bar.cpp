#include "screens/annotation_bar.h"

#include "platform/device_profile.h"
#include "ui/metrics.h"

#include <algorithm>

#include "docs/document.h"
#include "screens/reader.h"
#include "screens/screen.h"
#include "ui/text_field.h"
#include "ui/paint.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kSide = 40, kGap = 24, kPad = 20, kDotRowH = 130, kButtonH = 84;

}  // namespace

const std::array<ZoteroColour, 8>& zoteroColours() {
    static const std::array<ZoteroColour, 8> colours = {{{"#ffd400", "Yellow"},
                                                         {"#ff6666", "Red"},
                                                         {"#5fb236", "Green"},
                                                         {"#2ea8e5", "Blue"},
                                                         {"#a28ae5", "Purple"},
                                                         {"#e56eee", "Magenta"},
                                                         {"#f19837", "Orange"},
                                                         {"#aaaaaa", "Gray"}}};
    return colours;
}

AnnotationBar::AnnotationBar(Mode mode, Rect anchor, std::string colour)
    : mode_(mode), colour_(std::move(colour)) {
    const Rect page = ReaderScreen::pageArea();
    const int w = page.w - 2 * kSide;
    int y = anchor.y - kH - kGap;
    if (y < page.y + kGap) y = anchor.y + anchor.h + kGap;      // no room above: below
    if (y + kH > page.y + page.h - kGap) y = page.y + page.h - kGap - kH;  // and never off the page
    rect_ = {page.x + kSide, y, w, kH};
}

Rect AnnotationBar::dotRect(int i) const {
    const int w = (rect_.w - 2 * kPad) / 8;
    return {rect_.x + kPad + i * w, rect_.y + kPad, w, kDotRowH};
}

Rect AnnotationBar::buttonRect(int i) const {
    const int n = buttons();
    const int w = (rect_.w - 2 * kPad - (n - 1) * kGap) / n;
    return {rect_.x + kPad + i * (w + kGap), rect_.y + kPad + kDotRowH + 6, w, kButtonH};
}

void AnnotationBar::draw(cairo_t* cr) const {
    roundedRect(cr, rect_.x + 1.5, rect_.y + 1.5, rect_.w - 3, rect_.h - 3, 18);
    gray(cr, 255);
    cairo_fill_preserve(cr);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    for (int i = 0; i < 8; ++i) {
        const Rect d = dotRect(i);
        const auto& c = zoteroColours()[i];
        const double cx = d.x + d.w / 2.0, cy = d.y + 38;
        gray(cr, shadeOf(c.hex));
        cairo_arc(cr, cx, cy, 30, 0, 2 * 3.14159265);
        cairo_fill_preserve(cr);
        gray(cr, 120);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
        if (colour_ == c.hex) {  // the chosen colour: a ring around its dot
            gray(cr, kInk);
            cairo_set_line_width(cr, 5);
            cairo_arc(cr, cx, cy, 40, 0, 2 * 3.14159265);
            cairo_stroke(cr);
        }
        centredText(cr, c.name, {kSans, 6, colour_ == c.hex ? 700 : 400, false, kSecondary}, {d.x, d.y + 84, d.w, 40});
    }
    static const char* create[] = {"Highlight", "Underline", "Note"};
    static const char* edit[] = {"Comment", "Delete"};
    for (int i = 0; i < buttons(); ++i)
        drawButton(cr, buttonRect(i), mode_ == Mode::Create ? create[i] : edit[i], false);
}

AnnotationBar::Hit AnnotationBar::hit(Point p) const {
    for (int i = 0; i < 8; ++i)
        if (dotRect(i).contains(p)) return {Action::Colour, i};
    static const Action create[] = {Action::Highlight, Action::Underline, Action::Note};
    static const Action edit[] = {Action::Comment, Action::Delete};
    for (int i = 0; i < buttons(); ++i)
        if (buttonRect(i).contains(p)) return {mode_ == Mode::Create ? create[i] : edit[i], -1};
    return {};
}

AnnotationEditor::AnnotationEditor(AnnotationRecord a, Rect anchor)
    : annotation_(std::move(a)), bar_(AnnotationBar::Mode::Edit, anchor, annotation_.color) {}

// Just above the device's keyboard (on the PW5: {40, 690, 1156, 260}).
Rect AnnotationEditor::commentRect() {
    const int keyboard = profileFor(metrics().device, 300).keyboardHeightPx;
    const int top = keyboard > 0 ? metrics().height - keyboard : metrics().height / 2;
    return {40, top - 290, metrics().width - 80, 260};
}

Rect AnnotationEditor::rect() const { return state_ == State::Comment ? commentRect() : bar_.rect(); }

Rect AnnotationEditor::confirmRect(int i) const { return bar_.buttonRect(i); }

bool AnnotationEditor::takeChanged() {
    const bool c = changed_;
    changed_ = false;
    return c;
}

void AnnotationEditor::enter(State s, Nav& nav) {
    nav.damage(rect(), Waveform::Ui);
    if (s == State::Comment) {
        comment_ = annotation_.comment;
        nav.keyboard().show();
    } else if (state_ == State::Comment) {
        nav.keyboard().hide();
    }
    state_ = s;
    nav.damage(rect(), Waveform::Ui);
}

void AnnotationEditor::draw(cairo_t* cr) const {
    if (state_ == State::Bar) return bar_.draw(cr);
    const Rect r = rect();
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 18);
    gray(cr, 255);
    cairo_fill_preserve(cr);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    if (state_ == State::Confirm) {
        centredText(cr, "Delete this highlight?", {kSans, 9, 500}, {r.x + kPad, r.y + kPad, r.w - 2 * kPad, kDotRowH});
        drawButton(cr, confirmRect(0), "Delete", true);
        drawButton(cr, confirmRect(1), "Cancel", false);
        return;
    }
    drawText(cr, "Comment", {kSans, 8, 500}, {r.x + 30, r.y + 24, r.w - 60, 50}, 1);
    const bool empty = comment_.empty();
    drawText(cr, empty ? "Type a comment…" : comment_, {kSerif, 9, 400, false, empty ? kTertiary : kInk},
             {r.x + 30, r.y + 84, r.w - 60, 110}, 2);
    drawText(cr, "Return saves", {kSans, 6, 400, false, kTertiary}, {r.x + 30, r.y + r.h - 56, r.w - 60, 40}, 1, Align::Right);
}

AnnotationEditor::Result AnnotationEditor::onTap(Point p, Nav& nav) {
    switch (state_) {
        case State::Comment:
            if (commentRect().contains(p)) return Result::Open;
            nav.keyboard().hide();
            return Result::Closed;
        case State::Confirm:
            if (confirmRect(0).contains(p)) {
                nav.library().deleteAnnotation(annotation_.key);
                changed_ = true;
                return Result::Closed;
            }
            if (confirmRect(1).contains(p)) enter(State::Bar, nav);
            return bar_.rect().contains(p) ? Result::Open : Result::Closed;
        case State::Bar:
            break;
    }
    if (!bar_.rect().contains(p)) return Result::Closed;
    const AnnotationBar::Hit hit = bar_.hit(p);
    if (hit.action == AnnotationBar::Action::Colour) {
        const std::string colour = zoteroColours()[hit.colour].hex;
        // Change only the colour, on the annotation as it is now (a sync may have updated it).
        auto now = nav.library().annotation(annotation_.key);
        if (!now || now->deletedHere) return Result::Closed;
        annotation_ = *now;
        if (colour != annotation_.color) {
            annotation_.color = colour;
            nav.library().editAnnotation(annotation_);
            changed_ = true;
        }
        bar_.setColour(colour);
        nav.damage(bar_.rect(), Waveform::Ui);
    } else if (hit.action == AnnotationBar::Action::Comment) {
        enter(State::Comment, nav);
    } else if (hit.action == AnnotationBar::Action::Delete) {
        enter(State::Confirm, nav);
    }
    return Result::Open;
}

AnnotationEditor::Result AnnotationEditor::onKey(const KeyEvent& k, Nav& nav) {
    if (state_ != State::Comment) return Result::Open;
    if (k.code == KeyEvent::Code::Escape) {
        nav.keyboard().hide();
        return Result::Closed;
    }
    TextField field;
    field.setText(comment_);
    field.apply(k);
    comment_ = field.text();
    if (field.submitted()) {
        auto now = nav.library().annotation(annotation_.key);
        nav.keyboard().hide();
        if (!now || now->deletedHere) return Result::Closed;
        annotation_ = *now;
        annotation_.comment = comment_;
        nav.library().editAnnotation(annotation_);
        changed_ = true;
        return Result::Closed;
    }
    nav.damage(commentRect(), Waveform::Fast);
    return Result::Open;
}

}  // namespace zk
