#include "screens/search.h"

#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236, kH = 1547;
constexpr int kNoteH = 56;
constexpr int kMaxResults = 200;

}  // namespace

SearchScreen::SearchScreen(std::function<void(const ItemRow&)> openItem) : openItem_(std::move(openItem)) {}

Rect SearchScreen::fieldRect() { return {kMargin + 70, 30, kW - 2 * kMargin - 70, 92}; }

Rect SearchScreen::resultsArea(Nav& nav) const {
    const Rect kb = nav.keyboard().bounds();
    const int top = kHeaderH + kNoteH;
    const int bottom = kb.empty() ? kH : kb.y;
    return {0, top, kW, bottom - top};
}

std::string SearchScreen::note(Nav& nav) const {
    if (nav.library().allAnnotationsOffline()) return {};
    return "Highlights are searched only in books you've opened on the Kindle.";
}

std::string SearchScreen::emptyMessage() const {
    return searched_.empty() ? std::string{} : "Nothing found for “" + searched_ + "”";
}

void SearchScreen::onShow(Nav& nav) {
    nav.keyboard().show();
    noteCache_ = note(nav);
    areaCache_ = resultsArea(nav);
    list_.setRowsPerPage(ItemList::rowsFitting(areaCache_));
}

void SearchScreen::search(Nav& nav) {
    pending_ = false;
    const std::string q = field_.text();
    searched_ = q;
    areaCache_ = resultsArea(nav);
    list_.setRowsPerPage(ItemList::rowsFitting(areaCache_));
    list_.setSource([q](Library& lib, int64_t offset, int limit) { return lib.searchRows(q, static_cast<int>(offset), limit); },
                    [q](Library& lib) { return static_cast<int64_t>(lib.searchRows(q, 0, kMaxResults).size()); },
                    nav.library());
    nav.damage({0, kHeaderH, kW, kH - kHeaderH}, Waveform::Quality);
}

void SearchScreen::leave(Nav& nav) {
    nav.keyboard().hide();
    nav.pop();
}

void SearchScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (k.code == KeyEvent::Code::Escape) {
        leave(nav);
        return;
    }
    field_.apply(k);
    if (k.code == KeyEvent::Code::Return) {
        search(nav);
        return;
    }
    pending_ = true;
    lastKeyMs_ = nav.nowMs();
    nav.damage(fieldRect(), Waveform::Fast);
}

void SearchScreen::onTick(Nav& nav) {
    if (pending_ && nav.nowMs() - lastKeyMs_ >= kPauseMs) search(nav);
}

void SearchScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    drawHeader(cr, "", true);
    const Rect f = fieldRect();
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, f.x + 1.5, f.y + 1.5, f.w - 3, f.h - 3, f.h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, f.x + 54, f.y + f.h / 2.0 - 4, kTertiary);
    const bool empty = field_.text().empty();
    centredText(cr, empty ? "Search your library…" : field_.text() + "|", {kSans, 9, 400, false, empty ? kTertiary : kInk},
                {f.x + 96, f.y, f.w - 120, f.h}, Align::Left);
    if (!searched_.empty() && !noteCache_.empty())
        drawText(cr, noteCache_, {kSans, 6.5, 400, false, kTertiary}, {kMargin, kHeaderH + 6, kW - 2 * kMargin, kNoteH}, 1);
    if (!searched_.empty()) {
        list_.draw(cr, areaCache_);
        if (list_.rows().empty())
            drawText(cr, emptyMessage(), {kSerif, 11, 400, false, kSecondary}, {kMargin, areaCache_.y + 120, kW - 2 * kMargin, 100},
                     2, Align::Center);
    }
}

void SearchScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos) && !fieldRect().contains(g.pos)) {
        leave(nav);
        return;
    }
    if (g.type == GestureType::Tap && fieldRect().contains(g.pos)) {
        nav.keyboard().show();
        nav.redrawAll(Waveform::Quality);
        return;
    }
    list_.onGesture(g, areaCache_, nav, [&](const ItemRow& r) {
        nav.keyboard().hide();
        if (openItem_) openItem_(r);
    });
}

}  // namespace zk
