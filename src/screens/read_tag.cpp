#include "screens/read_tag.h"

#include <glib.h>

#include <algorithm>

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

std::vector<Rect> listRects(size_t count, int bottom) {
    std::vector<Rect> out;
    for (size_t i = 0; i < count; ++i) {
        const int y = kHeaderH + kNoteH + static_cast<int>(i) * kLineH;
        if (y + kLineH > bottom) break;
        out.push_back({kMargin, y, W() - 2 * kMargin, kLineH});
    }
    return out;
}

// The field (`text`, or `placeholder` greyed when empty), the note under it, and the tags.
void drawTagList(cairo_t* cr, const std::string& text, const std::string& placeholder, const std::string& note,
                 const std::vector<std::pair<std::string, int64_t>>& shown, const std::vector<Rect>& rects,
                 const std::string& unit = "item") {
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "", true);
    const Rect f = SearchScreen::fieldRect();
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, f.x + 1.5, f.y + 1.5, f.w - 3, f.h - 3, f.h / 2.0);
    cairo_stroke(cr);
    const bool empty = text.empty();
    centredText(cr, empty ? placeholder : text + "|", {kSans, 9, 400, false, empty ? kTertiary : kInk},
                {f.x + 50, f.y, f.w - 80, f.h}, Align::Left);
    drawText(cr, note, {kSans, 7.5, 400, false, kSecondary}, {kMargin, kHeaderH + 20, W() - 2 * kMargin, kNoteH - 30}, 2);
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect r = rects[i];
        centredText(cr, shown[i].first, {kSans, 9, 500}, {r.x, r.y, r.w - 200, r.h}, Align::Left);
        if (shown[i].second > 0)
            centredText(cr, std::to_string(shown[i].second) + " " + unit + (shown[i].second == 1 ? "" : "s"),
                        {kSans, 7, 400, false, kTertiary}, {r.x + r.w - 200, r.y, 200, r.h}, Align::Right);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
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

std::vector<Rect> ReadTagScreen::suggestionRects() const { return listRects(shown_.size(), bottom_); }

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
    drawTagList(canvas.cr(), field_.text(), kDefaultReadTag,
                "Items with this Zotero tag show as read. Mark as read adds it. Return keeps it.", shown_, suggestionRects());
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

TagPickerScreen::Options annotationTagFilter(const std::string& attachmentKey, bool reading) {
    TagPickerScreen::Options o;
    o.source = [attachmentKey](Library& lib) { return lib.annotationTags(attachmentKey); };
    o.note = "Show only the highlights and notes with this tag.";
    o.unit = "note";
    o.reading = reading;
    return o;
}

// ---- the Tag filter's picker ------------------------------------------------------------------

namespace {
std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\n") - a + 1);
}
}  // namespace

void TagPickerScreen::onShow(Nav& nav) {
    tags_ = options_.source ? options_.source(nav.library()) : nav.library().topTags(1 << 20);
    filter();
    nav.keyboard().show();
    const Rect kb = nav.keyboard().bounds();
    bottom_ = kb.empty() ? H() : kb.y;
}

void TagPickerScreen::filter() {
    shown_.clear();
    const std::string q = folded(field_.text());
    for (const auto& t : tags_)
        if (folded(t.first).find(q) != std::string::npos) shown_.push_back(t);
}

std::string TagPickerScreen::newTag() const {
    if (!options_.addNew) return "";
    const std::string t = trimmed(field_.text());
    if (t.empty()) return "";
    for (const auto& s : tags_)
        if (s.first == t) return "";
    return t;
}

Rect TagPickerScreen::addRect() const {
    if (newTag().empty()) return {};
    const auto r = listRects(1, bottom_);
    return r.empty() ? Rect{} : r[0];
}

std::vector<Rect> TagPickerScreen::suggestionRects() const {
    const size_t skip = newTag().empty() ? 0 : 1;
    auto rects = listRects(shown_.size() + skip, bottom_);
    rects.erase(rects.begin(), rects.begin() + static_cast<long>(std::min(skip, rects.size())));
    return rects;
}

void TagPickerScreen::choose(const std::string& tag, Nav& nav) {
    const std::string t = tag;  // before the pop: the screen under it repaints with the choice
    if (chosen_) chosen_(t);
    leave(nav);
}

void TagPickerScreen::leave(Nav& nav) {
    nav.keyboard().hide();
    nav.pop();
}

void TagPickerScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (k.code == KeyEvent::Code::Escape) return leave(nav);
    if (k.code == KeyEvent::Code::Return) {
        if (options_.addNew && !trimmed(field_.text()).empty()) return choose(trimmed(field_.text()), nav);
        if (!shown_.empty()) choose(shown_.front().first, nav);
        return;
    }
    field_.apply(k);
    filter();
    nav.damage({0, 0, W(), bottom_}, Waveform::Fast);
}

void TagPickerScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    drawTagList(cr, field_.text(), options_.placeholder, options_.note, shown_, suggestionRects(), options_.unit);
    if (const Rect r = addRect(); !r.empty()) {
        centredText(cr, "Add “" + newTag() + "”", {kSans, 9, 700}, {r.x, r.y, r.w, r.h}, Align::Left);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
}

void TagPickerScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (SearchScreen::fieldRect().contains(g.pos)) {
        nav.keyboard().show();
        nav.redrawAll(Waveform::Quality);
        return;
    }
    if (headerBackRect().contains(g.pos)) return leave(nav);
    if (const Rect r = addRect(); !r.empty() && r.contains(g.pos)) return choose(newTag(), nav);
    const auto rects = suggestionRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) return choose(shown_[i].first, nav);
}

}  // namespace zk
