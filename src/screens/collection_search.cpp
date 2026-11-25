#include "screens/collection_search.h"

#include <glib.h>

#include <algorithm>
#include <map>

#include "screens/collections.h"
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
constexpr int kLineH = 130;

std::string folded(const std::string& s) {
    gchar* f = g_utf8_casefold(s.c_str(), -1);
    std::string out = f ? f : "";
    g_free(f);
    return out;
}

}  // namespace

CollectionSearchScreen::CollectionSearchScreen(std::function<void(const ItemRow&)> openItem, StatusFn status)
    : openItem_(std::move(openItem)), status_(std::move(status)), bottom_(H()) {}

void CollectionSearchScreen::onShow(Nav& nav) {
    nav.keyboard().show();
    const Rect kb = nav.keyboard().bounds();
    bottom_ = kb.empty() ? H() : kb.y;
    page_ = std::min(page_, pages() - 1);
}

// All of them if they fit; else fewer, the page indicator under them.
int CollectionSearchScreen::perPage() const {
    const int room = bottom_ - (kHeaderH + 10);
    const int all = std::max(1, room / kLineH);
    if (static_cast<int>(shown_.size()) <= all) return all;
    return std::max(1, (room - kPageIndicatorH) / kLineH);
}

int CollectionSearchScreen::pages() const {
    const int n = static_cast<int>(shown_.size()), per = perPage();
    return std::max(1, (n + per - 1) / per);
}

Rect CollectionSearchScreen::indicatorRect() const { return {0, bottom_ - kPageIndicatorH, W(), kPageIndicatorH}; }

std::vector<Rect> CollectionSearchScreen::resultRects() const {
    std::vector<Rect> out;
    const size_t from = firstShown(), to = std::min(shown_.size(), from + static_cast<size_t>(perPage()));
    for (size_t i = from; i < to; ++i)
        out.push_back({kMargin, kHeaderH + 10 + static_cast<int>(i - from) * kLineH, W() - 2 * kMargin, kLineH});
    return out;
}

void CollectionSearchScreen::search(Nav& nav) {
    pending_ = false;
    searched_ = field_.text();
    shown_.clear();
    page_ = 0;
    const std::string q = folded(searched_);
    if (!q.empty()) {
        const auto all = nav.library().allCollections();
        std::map<std::string, const CollectionEntry*> byKey;
        for (const auto& e : all) byKey[e.collection.key] = &e;
        for (const auto& e : all) {
            if (folded(e.collection.name).find(q) == std::string::npos) continue;
            std::string path = e.collection.name;
            for (auto p = byKey.find(e.collection.parentKey); p != byKey.end(); p = byKey.find(p->second->collection.parentKey))
                path = p->second->collection.name + " › " + path;
            shown_.push_back({e, path});
        }
    }
    nav.damage({0, kHeaderH, W(), bottom_ - kHeaderH}, Waveform::Quality);
}

void CollectionSearchScreen::leave(Nav& nav) {
    nav.keyboard().hide();
    nav.pop();
}

void CollectionSearchScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (k.code == KeyEvent::Code::Escape) return leave(nav);
    field_.apply(k);
    if (k.code == KeyEvent::Code::Return) return search(nav);
    pending_ = true;
    lastKeyMs_ = nav.nowMs();
    nav.damage(SearchScreen::fieldRect(), Waveform::Fast);
}

void CollectionSearchScreen::onTick(Nav& nav) {
    if (pending_ && nav.nowMs() - lastKeyMs_ >= kPauseMs) search(nav);
    if (pending_) nav.wakeAt(lastKeyMs_ + kPauseMs);  // the search after the pause in typing
}

void CollectionSearchScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "", true);
    const Rect f = SearchScreen::fieldRect();
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, f.x + 1.5, f.y + 1.5, f.w - 3, f.h - 3, f.h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, f.x + 54, f.y + f.h / 2.0 - 4, kTertiary);
    const bool empty = field_.text().empty();
    centredText(cr, empty ? "Find a collection…" : field_.text() + "|", {kSans, 9, 400, false, empty ? kTertiary : kInk},
                {f.x + 96, f.y, f.w - 120, f.h}, Align::Left);
    const auto rects = resultRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const Result& r = shown_[firstShown() + i];
        drawCollectionRow(cr, rects[i], r.path, r.entry.items, r.entry.hasChildren, false, "", r.entry.kept);
    }
    if (pages() > 1) drawPageIndicator(cr, indicatorRect(), page_, pages());
    if (!searched_.empty() && shown_.empty())
        drawText(cr, "No collection called “" + searched_ + "”", {kSerif, 11, 400, false, kSecondary},
                 {kMargin, kHeaderH + 120, W() - 2 * kMargin, 100}, 2, Align::Center);
    if (sheet_) sheet_->draw(cr);
}

void CollectionSearchScreen::onGesture(const Gesture& g, Nav& nav) {
    if (sheet_) {  // any tap closes it; its buttons pin or keep, as in the Collections tab
        if (g.type != GestureType::Tap) return;
        sheet_->tap(g.pos, nav);
        sheet_.reset();
        const int page = page_;
        search(nav);  // the kept mark
        page_ = std::min(page, pages() - 1);
        nav.redrawAll(Waveform::Quality);
        return;
    }
    if (g.type == GestureType::LongPress) {
        const auto rects = resultRects();
        for (size_t i = 0; i < rects.size(); ++i)
            if (rects[i].contains(g.pos)) {
                const CollectionEntry& e = shown_[firstShown() + i].entry;
                const auto pins = nav.library().pinnedCollections();
                const bool pinned = std::find(pins.begin(), pins.end(), e.collection.key) != pins.end();
                nav.keyboard().hide();  // the sheet at the bottom, where the keyboard was
                bottom_ = H();
                page_ = std::min(page_, pages() - 1);
                sheet_.emplace(e, pinned, H());
                nav.redrawAll(Waveform::Ui);
                return;
            }
        return;
    }
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up || g.dir == Direction::Down)) {
        const int to = page_ + (g.dir == Direction::Up ? 1 : -1);
        if (to >= 0 && to < pages()) {
            page_ = to;
            nav.damage({0, kHeaderH, W(), bottom_ - kHeaderH}, Waveform::Quality);
        }
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos) && !SearchScreen::fieldRect().contains(g.pos)) return leave(nav);
    if (SearchScreen::fieldRect().contains(g.pos)) {
        onShow(nav);  // the keyboard back (hidden by a sheet), the results above it
        nav.redrawAll(Waveform::Quality);
        return;
    }
    const auto rects = resultRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) {
            nav.keyboard().hide();
            nav.push(std::make_unique<CollectionScreen>(shown_[firstShown() + i].entry, openItem_, status_));
            return;
        }
}

}  // namespace zk
