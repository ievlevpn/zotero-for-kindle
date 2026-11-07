#include "screens/collections.h"

#include "screens/collection_search.h"

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
constexpr int kLineH = 130;  // collection lines are shorter than item rows
Rect contentArea() { return {0, kHeaderH + 10, W(), H() - kTabBarH - kHeaderH - 10}; }

int collectionsPerPage() { return (contentArea().h - kPageIndicatorH) / kLineH; }

bool turnPage(const Gesture& g, Pager& pager) {
    if (g.type != GestureType::Swipe) return false;
    if (g.dir == Direction::Up) return pager.next();
    if (g.dir == Direction::Down) return pager.prev();
    return false;
}

}  // namespace

// ---- top level -------------------------------------------------------------------------------

CollectionsScreen::CollectionsScreen(std::function<void(const ItemRow&)> openItem, StatusFn status)
    : openItem_(std::move(openItem)), status_(std::move(status)) {}

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

Rect CollectionsScreen::searchRect() { return {W() - kMargin - 100, 20, 100, 100}; }

std::vector<Rect> CollectionsScreen::entryRects() const {
    std::vector<Rect> out;
    for (size_t i = 0; i < entries_.size(); ++i)
        out.push_back({kMargin, contentArea().y + static_cast<int>(i) * kLineH, W() - 2 * kMargin, kLineH});
    return out;
}

void CollectionsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Collections", false);
    const Rect s = searchRect();
    searchIcon(cr, s.x + s.w / 2.0, s.y + s.h / 2.0, kInk);
    auto rects = entryRects();
    for (size_t i = 0; i < entries_.size(); ++i)
        drawCollectionRow(cr, rects[i], entries_[i].collection.name, entries_[i].items, entries_[i].hasChildren);
    if (all_.empty())
        drawText(cr, "No collections yet.", {kSerif, 11, 400, false, kSecondary}, {kMargin, 500, W() - 2 * kMargin, 100}, 1,
                 Align::Center);
    drawPageIndicator(cr, {0, contentArea().y + contentArea().h - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void CollectionsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (turnPage(g, pager_)) {
        entries_.assign(all_.begin() + pager_.offset(),
                        all_.begin() + std::min<size_t>(all_.size(), pager_.offset() + pager_.perPage()));
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (searchRect().contains(g.pos)) {
        nav.push(std::make_unique<CollectionSearchScreen>(openItem_, status_));
        return;
    }
    auto rects = entryRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) nav.push(std::make_unique<CollectionScreen>(entries_[i], openItem_, status_));
}

void CollectionsScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    onShow(nav);
    nav.damage(contentArea(), Waveform::Quality);
}

// ---- one collection --------------------------------------------------------------------------

CollectionScreen::CollectionScreen(CollectionEntry entry, std::function<void(const ItemRow&)> openItem, StatusFn status)
    : entry_(std::move(entry)), openItem_(std::move(openItem)), status_(std::move(status)) {}

Rect CollectionScreen::backRect() { return headerBackRect(); }

// Subcollections take kLineH, items kRowH; a page is filled greedily in that order.
void CollectionScreen::load(Library& lib) {
    subs_ = lib.childCollections(entry_.collection.key);
    ListQuery q;
    q.collectionKey = entry_.collection.key;
    q.sort = SortBy::Title;
    const int64_t items = lib.countRows(q);
    // Paging counts lines; one "line" per subcollection or item, ~6 per page.
    const int perPage = (contentArea().h - kPageIndicatorH) / kRowH;
    const int64_t offset = pager_.offset();
    pager_ = Pager(static_cast<int64_t>(subs_.size()) + items, perPage);
    while (pager_.offset() < offset && pager_.next()) {
    }
    page_.clear();
    int64_t i = pager_.offset();
    int used = 0;
    const int room = contentArea().h - kPageIndicatorH;
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
    int y = contentArea().y;
    for (const auto& l : page_) {
        const int h = l.isCollection ? kLineH : kRowH;
        out.push_back({kMargin, y, W() - 2 * kMargin, h});
        y += h;
    }
    return out;
}

void CollectionScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, entry_.collection.name, true);
    auto rects = lineRects();
    for (size_t i = 0; i < page_.size(); ++i) {
        if (page_[i].isCollection)
            drawCollectionRow(cr, rects[i], page_[i].collection.collection.name, page_[i].collection.items,
                              page_[i].collection.hasChildren);
        else
            drawItemRow(cr, rects[i], page_[i].item, status_ ? status_(page_[i].item.attachmentKey) : "");
    }
    if (page_.empty())
        drawText(cr, "This collection is empty.", {kSerif, 11, 400, false, kSecondary}, {kMargin, 500, W() - 2 * kMargin, 100},
                 1, Align::Center);
    drawPageIndicator(cr, {0, contentArea().y + contentArea().h - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void CollectionScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap && backRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (turnPage(g, pager_)) {
        load(nav.library());
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    auto rects = lineRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(g.pos)) continue;
        if (page_[i].isCollection) nav.push(std::make_unique<CollectionScreen>(page_[i].collection, openItem_, status_));
        else if (openItem_) openItem_(page_[i].item);
    }
}

void CollectionScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind == SyncEvent::Kind::DownloadProgress || e.kind == SyncEvent::Kind::DownloadFinished) {
        if (e.kind == SyncEvent::Kind::DownloadFinished) load(nav.library());
        auto rects = lineRects();
        for (size_t i = 0; i < page_.size(); ++i)
            if (!page_[i].isCollection && page_[i].item.attachmentKey == e.attachmentKey) nav.damage(rects[i], Waveform::Ui);
        return;
    }
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    load(nav.library());
    nav.damage(contentArea(), Waveform::Quality);
}

}  // namespace zk
