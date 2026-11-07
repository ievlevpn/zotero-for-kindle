#include "screens/item_list.h"

#include "ui/paint.h"
#include "ui/widgets.h"

namespace zk {

int ItemList::rowsFitting(Rect area) { return std::max(1, (area.h - kPageIndicatorH) / kRowH); }

ItemList ItemList::forQuery(ListQuery q) {
    ItemList l;
    l.fetch_ = [q](Library& lib, int64_t offset, int limit) {
        ListQuery page = q;
        page.offset = static_cast<int>(offset);
        page.limit = limit;
        return lib.rows(page);
    };
    l.count_ = [q](Library& lib) { return lib.countRows(q); };
    return l;
}

void ItemList::setSource(Fetch f, Count c, Library& lib) {
    fetch_ = std::move(f);
    count_ = std::move(c);
    pager_ = Pager(count_ ? count_(lib) : 0, perPage_);
    load(lib);
}

void ItemList::refresh(Library& lib) {
    pager_.setTotal(count_ ? count_(lib) : 0);
    load(lib);
}

void ItemList::load(Library& lib) { rows_ = fetch_ ? fetch_(lib, pager_.offset(), perPage_) : std::vector<ItemRow>{}; }

std::vector<Rect> ItemList::rowRects(Rect area) const {
    std::vector<Rect> out;
    for (size_t i = 0; i < rows_.size(); ++i)
        out.push_back({area.x + kMargin, area.y + static_cast<int>(i) * kRowH, area.w - 2 * kMargin, kRowH});
    return out;
}

std::string ItemList::statusFor(const std::string& key) const { return statusFn_ && !key.empty() ? statusFn_(key) : std::string{}; }

void ItemList::onDownloadEvent(const SyncEvent& e, Rect area, Nav& nav) {
    if (e.kind != SyncEvent::Kind::DownloadProgress && e.kind != SyncEvent::Kind::DownloadFinished) return;
    const int i = indexOfAttachment(e.attachmentKey);
    if (i < 0) return;
    if (e.kind == SyncEvent::Kind::DownloadFinished) load(nav.library());  // the file state changed
    nav.damage(rowRects(area)[static_cast<size_t>(i)], Waveform::Fast);
}

int ItemList::indexOfAttachment(const std::string& key) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (!key.empty() && rows_[i].attachmentKey == key) return static_cast<int>(i);
    return -1;
}

void ItemList::draw(cairo_t* cr, Rect area) const {
    fillRect(cr, area, 255);
    auto rects = rowRects(area);
    for (size_t i = 0; i < rows_.size(); ++i) drawItemRow(cr, rects[i], rows_[i], statusFor(rows_[i].attachmentKey));
    drawPageIndicator(cr, {area.x, area.y + area.h - kPageIndicatorH, area.w, kPageIndicatorH}, pager_.page(),
                      pager_.pages());
}

bool ItemList::onGesture(const Gesture& g, Rect area, Nav& nav, const std::function<void(const ItemRow&)>& open) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up || g.dir == Direction::Down)) {
        if (g.dir == Direction::Up ? pager_.next() : pager_.prev()) {
            load(nav.library());
            nav.damage(area, Waveform::Quality);
        }
        return true;
    }
    if (g.type != GestureType::Tap) return false;
    auto rects = rowRects(area);
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) {
            if (open) open(rows_[i]);
            return true;
        }
    return false;
}

}  // namespace zk
