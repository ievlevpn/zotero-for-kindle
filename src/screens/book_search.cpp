#include "screens/book_search.h"

#include "screens/reader.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {
constexpr int kW = 1236, kHitH = 170;
const Rect kField{kMargin + 70, 30, kW - 2 * kMargin - 70, 92};
}  // namespace

BookSearchScreen::BookSearchScreen(ReaderScreen& r) : reader_(r) {
    reader_.setSearchListener([this](const std::vector<SearchHit>& hits) {
        hits_ = hits;
        searching_ = false;
        gotResults_ = true;
    });
}

BookSearchScreen::~BookSearchScreen() { reader_.setSearchListener(nullptr); }

void BookSearchScreen::onShow(Nav& nav) {
    nav.keyboard().show();
    const Rect kb = nav.keyboard().bounds();
    areaBottom_ = kb.empty() ? 1547 : kb.y;
}

void BookSearchScreen::onTick(Nav& nav) {
    reader_.pumpEvents(nav, false);  // results arrive through the reader's worker
    if (gotResults_) {
        gotResults_ = false;
        nav.damage({0, kHeaderH, kW, areaBottom_ - kHeaderH}, Waveform::Quality);
    }
}

std::vector<Rect> BookSearchScreen::hitRects() const {
    std::vector<Rect> out;
    for (size_t i = 0; i < hits_.size(); ++i) {
        const int y = kHeaderH + static_cast<int>(i) * kHitH;
        if (y + kHitH > areaBottom_) break;
        out.push_back({kMargin, y, kW - 2 * kMargin, kHitH});
    }
    return out;
}

void BookSearchScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, 1547}, 255);
    drawHeader(cr, "", true);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, kField.x + 1.5, kField.y + 1.5, kField.w - 3, kField.h - 3, kField.h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, kField.x + 54, kField.y + kField.h / 2.0 - 4, kTertiary);
    const bool empty = field_.text().empty();
    centredText(cr, empty ? "Search in this book…" : field_.text() + "|", {kSans, 9, 400, false, empty ? kTertiary : kInk},
                {kField.x + 96, kField.y, kField.w - 120, kField.h}, Align::Left);
    if (searching_) {
        drawText(cr, "Searching…", {kSerif, 10, 400, false, kSecondary}, {kMargin, kHeaderH + 120, kW - 2 * kMargin, 80}, 1,
                 Align::Center);
        return;
    }
    if (!emptyMessage().empty()) {
        drawText(cr, emptyMessage(), {kSerif, 10, 400, false, kSecondary}, {kMargin, kHeaderH + 120, kW - 2 * kMargin, 80}, 1,
                 Align::Center);
        return;
    }
    auto rects = hitRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect r = rects[i];
        drawText(cr, "p. " + std::to_string(hits_[i].page + 1), {kSans, 7, 500, false, kSecondary}, {r.x, r.y + 18, 200, 40}, 1);
        drawText(cr, hits_[i].snippet, {kSerif, 8.5, 400}, {r.x, r.y + 60, r.w, r.h - 70}, 2);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
}

void BookSearchScreen::leave(Nav& nav) {
    nav.keyboard().hide();
    nav.pop();
}

void BookSearchScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (k.code == KeyEvent::Code::Escape) {
        leave(nav);
        return;
    }
    field_.apply(k);
    if (k.code == KeyEvent::Code::Return && !field_.text().empty() && reader_.worker()) {
        searching_ = true;
        searched_ = true;
        hits_.clear();
        reader_.worker()->search(field_.text());
        nav.damage({0, kHeaderH, kW, areaBottom_ - kHeaderH}, Waveform::Quality);
        return;
    }
    nav.damage(kField, Waveform::Fast);
}

void BookSearchScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos) && !kField.contains(g.pos)) {
        leave(nav);
        return;
    }
    if (kField.contains(g.pos)) {
        nav.keyboard().show();
        return;
    }
    auto rects = hitRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) {
            const DocPosition pos = hits_[i].pos;
            ReaderScreen& r = reader_;
            leave(nav);
            r.jumpTo(pos);
            return;
        }
}

}  // namespace zk
