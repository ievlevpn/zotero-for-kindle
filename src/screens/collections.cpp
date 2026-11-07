#include "screens/collections.h"

#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236, kH = 1547;
constexpr int kLineH = 130;  // collection lines are shorter than item rows
const Rect kArea{0, kHeaderH + 10, kW, kH - kTabBarH - kHeaderH - 10};

int collectionsPerPage() { return (kArea.h - kPageIndicatorH) / kLineH; }

bool turnPage(const Gesture& g, Pager& pager) {
    if (g.type != GestureType::Swipe) return false;
    if (g.dir == Direction::Up) return pager.next();
    if (g.dir == Direction::Down) return pager.prev();
    return false;
}

}  // namespace

// ---- top level -------------------------------------------------------------------------------

CollectionsScreen::CollectionsScreen(std::function<void(const ItemRow&)> openItem) : openItem_(std::move(openItem)) {}

void CollectionsScreen::load(Library& lib) {
    all_ = lib.childCollections("");
    pager_ = Pager(static_cast<int64_t>(all_.size()), collectionsPerPage());
    entries_.assign(all_.begin(), all_.begin() + std::min<size_t>(all_.size(), collectionsPerPage()));
}

void CollectionsScreen::onShow(Nav& nav) {
    const int64_t offset = pager_.offset();
    load(nav.library());
    while (pager_.offset() < offset && pager_.next()) {
    }
    entries_.assign(all_.begin() + pager_.offset(),
                    all_.begin() + std::min<size_t>(all_.size(), pager_.offset() + pager_.perPage()));
}

std::vector<Rect> CollectionsScreen::entryRects() const {
    std::vector<Rect> out;
    for (size_t i = 0; i < entries_.size(); ++i)
        out.push_back({kMargin, kArea.y + static_cast<int>(i) * kLineH, kW - 2 * kMargin, kLineH});
    return out;
}

void CollectionsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    drawHeader(cr, "Collections", false);
    auto rects = entryRects();
    for (size_t i = 0; i < entries_.size(); ++i)
        drawCollectionRow(cr, rects[i], entries_[i].collection.name, entries_[i].items, entries_[i].hasChildren);
    if (all_.empty())
        drawText(cr, "No collections yet.", {kSerif, 11, 400, false, kSecondary}, {kMargin, 500, kW - 2 * kMargin, 100}, 1,
                 Align::Center);
    drawPageIndicator(cr, {0, kArea.y + kArea.h - kPageIndicatorH, kW, kPageIndicatorH}, pager_.page(), pager_.pages());
}

void CollectionsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (turnPage(g, pager_)) {
        entries_.assign(all_.begin() + pager_.offset(),
                        all_.begin() + std::min<size_t>(all_.size(), pager_.offset() + pager_.perPage()));
        nav.damage(kArea, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    auto rects = entryRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) nav.push(std::make_unique<CollectionScreen>(entries_[i], openItem_));
}

void CollectionsScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    onShow(nav);
    nav.damage(kArea, Waveform::Quality);
}

// ---- one collection --------------------------------------------------------------------------

CollectionScreen::CollectionScreen(CollectionEntry entry, std::function<void(const ItemRow&)> openItem)
    : entry_(std::move(entry)), openItem_(std::move(openItem)) {}

Rect CollectionScreen::backRect() { return headerBackRect(); }

// Subcollections take kLineH, items kRowH; a page is filled greedily in that order.
void CollectionScreen::load(Library& lib) {
    subs_ = lib.childCollections(entry_.collection.key);
    ListQuery q;
    q.collectionKey = entry_.collection.key;
    q.sort = SortBy::Title;
    const int64_t items = lib.countRows(q);
    // Paging counts lines; one "line" per subcollection or item, ~6 per page.
    const int perPage = (kArea.h - kPageIndicatorH) / kRowH;
    const int64_t offset = pager_.offset();
    pager_ = Pager(static_cast<int64_t>(subs_.size()) + items, perPage);
    while (pager_.offset() < offset && pager_.next()) {
    }
    page_.clear();
    int64_t i = pager_.offset();
    int used = 0;
    const int room = kArea.h - kPageIndicatorH;
    for (; i < static_cast<int64_t>(subs_.size()) && used + kLineH <= room; ++i, used += kLineH)
        page_.push_back({true, subs_[static_cast<size_t>(i)], {}});
    if (used + kRowH <= room) {
        const int itemSlots = (room - used) / kRowH;
        q.offset = static_cast<int>(std::max<int64_t>(0, i - static_cast<int64_t>(subs_.size())));
        q.limit = itemSlots;
        for (auto& r : lib.rows(q)) page_.push_back({false, {}, std::move(r)});
    }
}

void CollectionScreen::onShow(Nav& nav) { load(nav.library()); }

std::vector<Rect> CollectionScreen::lineRects() const {
    std::vector<Rect> out;
    int y = kArea.y;
    for (const auto& l : page_) {
        const int h = l.isCollection ? kLineH : kRowH;
        out.push_back({kMargin, y, kW - 2 * kMargin, h});
        y += h;
    }
    return out;
}

void CollectionScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    drawHeader(cr, entry_.collection.name, true);
    auto rects = lineRects();
    for (size_t i = 0; i < page_.size(); ++i) {
        if (page_[i].isCollection)
            drawCollectionRow(cr, rects[i], page_[i].collection.collection.name, page_[i].collection.items,
                              page_[i].collection.hasChildren);
        else
            drawItemRow(cr, rects[i], page_[i].item, "");
    }
    if (page_.empty())
        drawText(cr, "This collection is empty.", {kSerif, 11, 400, false, kSecondary}, {kMargin, 500, kW - 2 * kMargin, 100},
                 1, Align::Center);
    drawPageIndicator(cr, {0, kArea.y + kArea.h - kPageIndicatorH, kW, kPageIndicatorH}, pager_.page(), pager_.pages());
}

void CollectionScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap && backRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (turnPage(g, pager_)) {
        load(nav.library());
        nav.damage(kArea, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    auto rects = lineRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(g.pos)) continue;
        if (page_[i].isCollection) nav.push(std::make_unique<CollectionScreen>(page_[i].collection, openItem_));
        else if (openItem_) openItem_(page_[i].item);
    }
}

void CollectionScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    load(nav.library());
    nav.damage(kArea, Waveform::Quality);
}

}  // namespace zk
