#include "screens/annotation_bar.h"

#include <algorithm>

#include "docs/document.h"
#include "screens/reader.h"
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

}  // namespace zk
