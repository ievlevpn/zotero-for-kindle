#include "screens/read_tag.h"

#include <glib.h>

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
constexpr int kNoteH = 130, kLineH = 110, kTags = 500;

std::string folded(const std::string& s) {
    gchar* f = g_utf8_casefold(s.c_str(), -1);
    std::string out = f ? f : "";
    g_free(f);
    return out;
}

}  // namespace

void ReadTagScreen::onShow(Nav& nav) {
    field_.setText(nav.library().readTag());
    tags_ = nav.library().topTags(kTags);
    filter();
    nav.keyboard().show();
    const Rect kb = nav.keyboard().bounds();
    bottom_ = kb.empty() ? H() : kb.y;
}

void ReadTagScreen::filter() {
    shown_.clear();
    const std::string q = folded(field_.text());
    for (const auto& t : tags_)
        if (t.first != field_.text() && folded(t.first).find(q) != std::string::npos) shown_.push_back(t);
}

std::vector<Rect> ReadTagScreen::suggestionRects() const {
    std::vector<Rect> out;
    for (size_t i = 0; i < shown_.size(); ++i) {
        const int y = kHeaderH + kNoteH + static_cast<int>(i) * kLineH;
        if (y + kLineH > bottom_) break;
        out.push_back({kMargin, y, W() - 2 * kMargin, kLineH});
    }
    return out;
}

void ReadTagScreen::save(const std::string& tag, Nav& nav) {
    nav.library().setReadTag(tag);
    leave(nav);
}

void ReadTagScreen::leave(Nav& nav) {
    nav.keyboard().hide();
    nav.pop();
}

void ReadTagScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (k.code == KeyEvent::Code::Escape) return leave(nav);
    if (k.code == KeyEvent::Code::Return) return save(field_.text(), nav);
    field_.apply(k);
    filter();
    nav.damage({0, 0, W(), bottom_}, Waveform::Fast);
}

void ReadTagScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "", true);
    const Rect f = SearchScreen::fieldRect();
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, f.x + 1.5, f.y + 1.5, f.w - 3, f.h - 3, f.h / 2.0);
    cairo_stroke(cr);
    const bool empty = field_.text().empty();
    centredText(cr, empty ? std::string(kDefaultReadTag) : field_.text() + "|",
                {kSans, 9, 400, false, empty ? kTertiary : kInk}, {f.x + 50, f.y, f.w - 80, f.h}, Align::Left);
    drawText(cr, "Items with this Zotero tag show as read. Mark as read adds it. Return keeps it.",
             {kSans, 7.5, 400, false, kSecondary}, {kMargin, kHeaderH + 20, W() - 2 * kMargin, kNoteH - 30}, 2);
    const auto rects = suggestionRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect r = rects[i];
        centredText(cr, shown_[i].first, {kSans, 9, 500}, {r.x, r.y, r.w - 200, r.h}, Align::Left);
        centredText(cr, std::to_string(shown_[i].second) + (shown_[i].second == 1 ? " item" : " items"),
                    {kSans, 7, 400, false, kTertiary}, {r.x + r.w - 200, r.y, 200, r.h}, Align::Right);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
}

void ReadTagScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (SearchScreen::fieldRect().contains(g.pos)) {
        nav.keyboard().show();
        nav.redrawAll(Waveform::Quality);
        return;
    }
    if (headerBackRect().contains(g.pos)) return leave(nav);
    const auto rects = suggestionRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) return save(shown_[i].first, nav);
}

}  // namespace zk
