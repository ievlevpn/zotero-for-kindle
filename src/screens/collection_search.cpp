#include "screens/collection_search.h"

#include <glib.h>

#include <map>

#include "screens/collections.h"
#include "screens/search.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236, kLineH = 130;

std::string folded(const std::string& s) {
    gchar* f = g_utf8_casefold(s.c_str(), -1);
    std::string out = f ? f : "";
    g_free(f);
    return out;
}

}  // namespace

CollectionSearchScreen::CollectionSearchScreen(std::function<void(const ItemRow&)> openItem, StatusFn status)
    : openItem_(std::move(openItem)), status_(std::move(status)) {}

void CollectionSearchScreen::onShow(Nav& nav) {
    nav.keyboard().show();
    const Rect kb = nav.keyboard().bounds();
    bottom_ = kb.empty() ? 1547 : kb.y;
}

std::vector<Rect> CollectionSearchScreen::resultRects() const {
    std::vector<Rect> out;
    for (size_t i = 0; i < shown_.size(); ++i) {
        const int y = kHeaderH + 10 + static_cast<int>(i) * kLineH;
        if (y + kLineH > bottom_) break;
        out.push_back({kMargin, y, kW - 2 * kMargin, kLineH});
    }
    return out;
}

void CollectionSearchScreen::search(Nav& nav) {
    pending_ = false;
    searched_ = field_.text();
    shown_.clear();
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
    nav.damage({0, kHeaderH, kW, bottom_ - kHeaderH}, Waveform::Quality);
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
}

void CollectionSearchScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, 1547}, 255);
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
    for (size_t i = 0; i < rects.size(); ++i)
        drawCollectionRow(cr, rects[i], shown_[i].path, shown_[i].entry.items, shown_[i].entry.hasChildren);
    if (!searched_.empty() && shown_.empty())
        drawText(cr, "No collection called “" + searched_ + "”", {kSerif, 11, 400, false, kSecondary},
                 {kMargin, kHeaderH + 120, kW - 2 * kMargin, 100}, 2, Align::Center);
}

void CollectionSearchScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos) && !SearchScreen::fieldRect().contains(g.pos)) return leave(nav);
    if (SearchScreen::fieldRect().contains(g.pos)) {
        nav.keyboard().show();
        nav.redrawAll(Waveform::Quality);
        return;
    }
    const auto rects = resultRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) {
            nav.keyboard().hide();
            nav.push(std::make_unique<CollectionScreen>(shown_[i].entry, openItem_, status_));
            return;
        }
}

}  // namespace zk
