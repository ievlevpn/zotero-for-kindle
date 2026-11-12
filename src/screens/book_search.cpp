#include "screens/book_search.h"

#include "screens/reader.h"
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
constexpr int kHitH = 170;
Rect field() { return {kMargin + 70, 30, W() - 2 * kMargin - 70, 92}; }
}  // namespace

BookSearchScreen::BookSearchScreen(ReaderScreen& r, std::string query) : reader_(r), pending_(std::move(query)) {
    reader_.setSearchListener([this](uint64_t id, const std::vector<SearchHit>& hits) {
        if (id != requestId_) return;
        hits_ = hits;
        searching_ = false;
        gotResults_ = true;
    });
}

BookSearchScreen::~BookSearchScreen() { reader_.setSearchListener(nullptr); }

std::string BookSearchScreen::emptyMessage() const {
    if (!searched_ || !hits_.empty()) return "";
    return reader_.webPage() ? "Not found on this page." : "Not found in this book.";
}

void BookSearchScreen::onShow(Nav& nav) {
    if (!pending_.empty()) {  // the selection's words: their hits, the whole screen for them
        field_.setText(std::move(pending_));
        pending_.clear();
        areaBottom_ = H();
        run(nav);
        return;
    }
    nav.keyboard().show();
    const Rect kb = nav.keyboard().bounds();
    areaBottom_ = kb.empty() ? H() : kb.y;
}

void BookSearchScreen::run(Nav& nav) {
    if (field_.text().empty() || !reader_.worker()) return;
    searching_ = true;
    searched_ = true;
    hits_.clear();
    requestId_ = reader_.worker()->search(field_.text());
    nav.damage({0, kHeaderH, W(), areaBottom_ - kHeaderH}, Waveform::Quality);
}

void BookSearchScreen::onTick(Nav& nav) {
    reader_.pumpEvents(nav, false);  // results arrive through the reader's worker
    if (gotResults_) {
        gotResults_ = false;
        nav.damage({0, kHeaderH, W(), areaBottom_ - kHeaderH}, Waveform::Quality);
    }
}

std::vector<Rect> BookSearchScreen::hitRects() const {
    std::vector<Rect> out;
    for (size_t i = 0; i < hits_.size(); ++i) {
        const int y = kHeaderH + static_cast<int>(i) * kHitH;
        if (y + kHitH > areaBottom_) break;
        out.push_back({kMargin, y, W() - 2 * kMargin, kHitH});
    }
    return out;
}

void BookSearchScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "", true);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, field().x + 1.5, field().y + 1.5, field().w - 3, field().h - 3, field().h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, field().x + 54, field().y + field().h / 2.0 - 4, kTertiary);
    const bool empty = field_.text().empty();
    centredText(cr, empty ? (reader_.webPage() ? "Search in this page…" : "Search in this book…") : field_.text() + "|", {kSans, 9, 400, false, empty ? kTertiary : kInk},
                {field().x + 96, field().y, field().w - 120, field().h}, Align::Left);
    if (searching_) {
        drawText(cr, "Searching…", {kSerif, 10, 400, false, kSecondary}, {kMargin, kHeaderH + 120, W() - 2 * kMargin, 80}, 1,
                 Align::Center);
        return;
    }
    if (!emptyMessage().empty()) {
        drawText(cr, emptyMessage(), {kSerif, 10, 400, false, kSecondary}, {kMargin, kHeaderH + 120, W() - 2 * kMargin, 80}, 1,
                 Align::Center);
        return;
    }
    auto rects = hitRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect r = rects[i];
        drawText(cr, "p. " + std::to_string(reader_.shownPage(hits_[i].page)), {kSans, 7, 500, false, kSecondary}, {r.x, r.y + 18, 200, 40}, 1);
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
    if (k.code == KeyEvent::Code::Return && !field_.text().empty() && reader_.worker()) return run(nav);
    nav.damage(field(), Waveform::Fast);
}

void BookSearchScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos) && !field().contains(g.pos)) {
        leave(nav);
        return;
    }
    if (field().contains(g.pos)) {
        nav.keyboard().show();
        const Rect kb = nav.keyboard().bounds();
        if (!kb.empty() && kb.y != areaBottom_) {  // opened without it (a selection's Search): hits above it now
            areaBottom_ = kb.y;
            nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
        }
        return;
    }
    auto rects = hitRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) {
            const SearchHit hit = hits_[i];
            ReaderScreen& r = reader_;
            leave(nav);
            r.jumpToHit(hit);
            return;
        }
}

}  // namespace zk
