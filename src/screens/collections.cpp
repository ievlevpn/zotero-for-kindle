#include "screens/collections.h"

#include <algorithm>

#include "screens/collection_search.h"
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
constexpr int kLineH = 130;  // collection lines are shorter than item rows
Rect contentArea() { return {0, kHeaderH + 10, W(), H() - kTabBarH - kHeaderH - 10}; }

int collectionsPerPage() { return (contentArea().h - kPageIndicatorH) / kLineH; }

// By name as the Library sorts titles (listSortKey): the initials run together, # last.
void sortByName(std::vector<CollectionEntry>::iterator from, std::vector<CollectionEntry>::iterator to) {
    std::vector<std::pair<std::string, CollectionEntry>> keyed;
    for (auto it = from; it != to; ++it) keyed.emplace_back(listSortKey(it->collection.name), std::move(*it));
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& k : keyed) *from++ = std::move(k.second);
}

std::vector<std::string> namesOf(std::vector<CollectionEntry>::const_iterator from, std::vector<CollectionEntry>::const_iterator to) {
    std::vector<std::string> out;
    for (; from != to; ++from) out.push_back(from->collection.name);
    return out;
}

bool turnPage(const Gesture& g, Pager& pager) {
    if (g.type != GestureType::Swipe) return false;
    if (g.dir == Direction::Up) return pager.next();
    if (g.dir == Direction::Down) return pager.prev();
    return false;
}

}  // namespace

// ---- the pin sheet ---------------------------------------------------------------------------

Rect PinSheet::rect() { return {0, H() - kTabBarH - 420, W(), 420}; }
Rect PinSheet::buttonRect() { return {kMargin, rect().y + 150, W() - 2 * kMargin, 100}; }
Rect PinSheet::keepButtonRect() { return {kMargin, rect().y + 280, W() - 2 * kMargin, 100}; }

void PinSheet::draw(cairo_t* cr) const {
    const Rect r = rect();
    fillRect(cr, r, 255);
    fillRect(cr, {r.x, r.y, r.w, 3}, kInk);
    drawText(cr, entry_.collection.name.empty() ? "Untitled" : entry_.collection.name, {kSans, 9.5, 700},
             {kMargin, r.y + 40, r.w - 2 * kMargin, 60}, 1);
    drawButton(cr, buttonRect(), label(), false);
    drawButton(cr, keepButtonRect(), keepLabel(), false);
}

void PinSheet::tap(Point p, Nav& nav) const {
    if (buttonRect().contains(p)) nav.library().setCollectionPinned(entry_.collection.key, !pinned_);
    if (keepButtonRect().contains(p)) {
        nav.library().setCollectionKept(entry_.collection.key, !entry_.kept);
        if (!entry_.kept) nav.sync().downloadKept();  // the missing files, now (if Wi-Fi is up)
    }
}

std::string keptNote(const std::optional<KeptStatus>& st) {
    if (!st) return {};
    if (st->paused) return "Paused — the Kindle is nearly full";
    return "Downloading " + std::to_string(st->current) + " of " + std::to_string(st->total);
}

namespace {

// The status of each kept collection among `entries` that is catching up.
std::map<std::string, KeptStatus> keptStatuses(const std::vector<CollectionEntry>& entries, Nav& nav) {
    std::map<std::string, KeptStatus> out;
    for (const auto& e : entries)
        if (e.kept)
            if (auto st = nav.sync().keptStatus(e.collection.key)) out[e.collection.key] = *st;
    return out;
}

std::optional<KeptStatus> statusOf(const std::map<std::string, KeptStatus>& kept, const std::string& key) {
    auto it = kept.find(key);
    return it == kept.end() ? std::nullopt : std::optional(it->second);
}

// "in Economics", "Downloading 3 of 12", or both.
std::string joinNotes(const std::string& a, const std::string& b) { return a.empty() ? b : b.empty() ? a : a + " · " + b; }

}  // namespace

// ---- top level -------------------------------------------------------------------------------

CollectionsScreen::CollectionsScreen(std::function<void(const ItemRow&)> openItem, StatusFn status)
    : openItem_(std::move(openItem)), status_(std::move(status)) {}

Rect CollectionsScreen::footerRect() { return {0, contentArea().y + contentArea().h - kPageIndicatorH, W(), kPageIndicatorH}; }
Rect CollectionsScreen::sheetArea() { return {0, kHeaderH, W(), H() - kTabBarH - kHeaderH}; }

void CollectionsScreen::load(Library& lib) {
    all_ = lib.collectionsTab();  // the pinned ones first, as pinned; the jump sheet has the others' initials
    const auto pinned = std::find_if(all_.begin(), all_.end(), [](const CollectionEntry& e) { return !e.pinned; });
    sortByName(pinned, all_.end());
    sections_ = nameSections(namesOf(pinned, all_.end()), &starts_);
    for (auto& s : starts_) s += pinned - all_.begin();
    groups_.clear();
    for (auto& l : lib.libraries(false))  // those shown, by name
        if (l.id != kMyLibrary) groups_.push_back(std::move(l));
    if (!groups_.empty()) starts_.push_back(static_cast<int64_t>(all_.size()));  // the jump sheet's "Groups": the heading
    pager_ = Pager(lines(), collectionsPerPage());
    showPage();
}

void CollectionsScreen::showPage() {
    const auto from = std::min<size_t>(all_.size(), static_cast<size_t>(pager_.offset()));
    entries_.assign(all_.begin() + from, all_.begin() + std::min<size_t>(all_.size(), pager_.offset() + pager_.perPage()));
}

void CollectionsScreen::onShow(Nav& nav) {
    sheet_.reset();  // a long press's sheet doesn't outlast a visit to another tab
    jump_.reset();
    const int64_t offset = pager_.offset();
    load(nav.library());
    while (pager_.offset() < offset && pager_.next()) {
    }
    showPage();
    loadKept(nav);
}

void CollectionsScreen::loadKept(Nav& nav) { kept_ = keptStatuses(all_, nav); }

std::string CollectionsScreen::rowNote(size_t i) const {
    const auto& e = entries_.at(i);
    std::string where = e.parentName;
    if (where.empty() && e.collection.lib != kMyLibrary)  // a group's pinned top-level collection: its group
        for (const auto& g : groups_)
            if (g.id == e.collection.lib) where = g.name;
    return joinNotes(where.empty() ? "" : "in " + where, keptNote(statusOf(kept_, e.collection.key)));
}

Rect CollectionsScreen::searchRect() { return {W() - kMargin - 100, 20, 100, 100}; }

Rect CollectionsScreen::lineRect(int64_t line) const {
    return {kMargin, contentArea().y + static_cast<int>(line - pager_.offset()) * kLineH, W() - 2 * kMargin, kLineH};
}

std::vector<Rect> CollectionsScreen::entryRects() const {
    std::vector<Rect> out;
    for (size_t i = 0; i < entries_.size(); ++i) out.push_back(lineRect(pager_.offset() + static_cast<int64_t>(i)));
    return out;
}

std::optional<Rect> CollectionsScreen::groupHeadingRect() const {
    const auto line = static_cast<int64_t>(all_.size());
    if (groups_.empty() || line < pager_.offset() || line >= pager_.offset() + pager_.perPage()) return std::nullopt;
    return lineRect(line);
}

std::vector<std::pair<LibraryInfo, Rect>> CollectionsScreen::groupRows() const {
    std::vector<std::pair<LibraryInfo, Rect>> out;
    for (size_t j = 0; j < groups_.size(); ++j) {
        const auto line = static_cast<int64_t>(all_.size() + 1 + j);
        if (line >= pager_.offset() && line < pager_.offset() + pager_.perPage()) out.emplace_back(groups_[j], lineRect(line));
    }
    return out;
}

void CollectionsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Collections", false);
    const Rect s = searchRect();
    searchIcon(cr, s.x + s.w / 2.0, s.y + s.h / 2.0, kInk);
    auto rects = entryRects();
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        drawCollectionRow(cr, rects[i], e.collection.name, e.items, e.hasChildren, e.pinned, rowNote(i), e.kept);
    }
    if (auto h = groupHeadingRect()) drawSectionHeading(cr, *h, "Group libraries");
    for (const auto& [g, r] : groupRows()) drawGroupRow(cr, r, g.name, g.items);
    if (all_.empty() && groups_.empty())
        drawText(cr, "No collections yet.", {kSerif, 11, 400, false, kSecondary}, {kMargin, 500, W() - 2 * kMargin, 100}, 1,
                 Align::Center);
    drawJumpIndicator(cr, footerRect(), pager_.page(), pager_.pages());
    if (sheet_) sheet_->draw(cr);
    if (jump_) jump_->draw(cr, sheetArea());
}

void CollectionsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (jump_) {
        Nav* n = &nav;
        return jumpSheetGesture(
            jump_, g, sheetArea(), nav, pager_.page(), pager_.pages(), [this](size_t s) { return pager_.pageOf(starts_.at(s)); },
            [this, n](int page) {
                if (pager_.goTo(page)) showPage();
                n->damage(contentArea(), Waveform::Quality);
            });
    }
    if (sheet_) {  // any tap closes it; its button pins or unpins
        if (g.type != GestureType::Tap) return;
        const bool pinning = sheet_->label() == "Pin to top" && PinSheet::buttonRect().contains(g.pos);
        sheet_->tap(g.pos, nav);
        if (pinning) pager_ = Pager(0, 1);  // to the first page, where it now is
        onShow(nav);
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (turnPage(g, pager_)) {
        showPage();
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (g.type == GestureType::LongPress) {
        auto rects = entryRects();
        for (size_t i = 0; i < rects.size(); ++i)
            if (rects[i].contains(g.pos)) {
                sheet_.emplace(entries_[i], entries_[i].pinned);
                nav.damage(PinSheet::rect(), Waveform::Ui);
            }
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (searchRect().contains(g.pos)) {
        nav.push(std::make_unique<CollectionSearchScreen>(openItem_, status_));
        return;
    }
    if (footerRect().contains(g.pos)) {
        if (pager_.pages() > 1) {
            auto index = jumpGroups(SortBy::Title, sections_);
            if (!groups_.empty()) index.push_back({"", {{"Groups", sections_.size()}}});  // to the heading's page
            jump_.emplace(std::move(index), pager_.page(), pager_.pages());
            nav.damage(sheetArea(), Waveform::Ui);
        }
        return;
    }
    auto rects = entryRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) nav.push(std::make_unique<CollectionScreen>(entries_[i], openItem_, status_));
    for (const auto& [lib, r] : groupRows())
        if (r.contains(g.pos)) nav.push(std::make_unique<CollectionScreen>(lib, openItem_, status_));
}

void CollectionsScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind == SyncEvent::Kind::KeptChanged) {  // only the rows whose note changed
        const auto was = kept_;
        loadKept(nav);
        auto rects = entryRects();
        for (size_t i = 0; i < entries_.size(); ++i) {
            const std::string& key = entries_[i].collection.key;
            // The note's last change (gone) rests: the quality waveform, no ghost of "Downloading…".
            if (statusOf(was, key) != statusOf(kept_, key))
                nav.damage(rects[i], statusOf(kept_, key) ? Waveform::Ui : Waveform::Quality);
        }
        return;
    }
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    onShow(nav);
    nav.damage(contentArea(), Waveform::Quality);
}

// ---- one collection --------------------------------------------------------------------------

CollectionScreen::CollectionScreen(CollectionEntry entry, std::function<void(const ItemRow&)> openItem, StatusFn status)
    : entry_(std::move(entry)), openItem_(std::move(openItem)), status_(std::move(status)) {}

CollectionScreen::CollectionScreen(const LibraryInfo& group, std::function<void(const ItemRow&)> openItem, StatusFn status)
    : group_(group.id), openItem_(std::move(openItem)), status_(std::move(status)) {
    entry_.collection.name = group.name;
    entry_.collection.lib = group.id;
    entry_.items = group.items;
}

Rect CollectionScreen::backRect() { return headerBackRect(); }
Rect CollectionScreen::searchRect() { return CollectionsScreen::searchRect(); }

// Subcollections take kLineH, items kRowH; a page is filled greedily in that order.
void CollectionScreen::load(Library& lib) {
    subs_ = group_ ? lib.childCollections("", group_) : lib.childCollections(entry_.collection.key);
    sortByName(subs_.begin(), subs_.end());
    ListQuery q;
    q.collectionKey = entry_.collection.key;  // a group's list: "", with the library
    if (group_) q.lib = group_;
    q.sort = SortBy::Title;
    const int64_t items = lib.countRows(q);
    subSections_ = nameSections(namesOf(subs_.begin(), subs_.end()), &starts_);
    itemSections_.clear();
    int64_t start = static_cast<int64_t>(subs_.size());
    for (const auto& c : lib.sectionCounts(q)) {
        itemSections_.push_back({c.key, sectionLabel(SortBy::Title, c.key), c.count});
        starts_.push_back(start);
        start += c.count;
    }
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

void CollectionScreen::onShow(Nav& nav) {
    sheet_.reset();
    jump_.reset();
    load(nav.library());
    loadKept(nav);
}

void CollectionScreen::loadKept(Nav& nav) { kept_ = keptStatuses(subs_, nav); }

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
    drawHeader(cr, entry_.collection.name, true, searchRect().w + 20);
    const Rect s = searchRect();
    if (!group_) searchIcon(cr, s.x + s.w / 2.0, s.y + s.h / 2.0, kInk);  // search is by collection
    auto rects = lineRects();
    for (size_t i = 0; i < page_.size(); ++i) {
        if (page_[i].isCollection)
            drawCollectionRow(cr, rects[i], page_[i].collection.collection.name, page_[i].collection.items,
                              page_[i].collection.hasChildren, false, keptNote(statusOf(kept_, page_[i].collection.collection.key)),
                              page_[i].collection.kept);
        else
            drawItemRow(cr, rects[i], page_[i].item, status_ ? status_(page_[i].item.attachmentKey) : "");
    }
    if (page_.empty())
        drawText(cr, group_ ? "This group library is empty." : "This collection is empty.", {kSerif, 11, 400, false, kSecondary}, {kMargin, 500, W() - 2 * kMargin, 100},
                 1, Align::Center);
    drawJumpIndicator(cr, footerRect(), pager_.page(), pager_.pages());
    if (sheet_) sheet_->draw(cr);
    if (jump_) jump_->draw(cr, sheetArea());
}

// The subcollections' initials, then the items'; under "Collections" and "Items" when there are both.
std::vector<JumpGroup> CollectionScreen::jumpIndex() const {
    auto subs = jumpGroups(SortBy::Title, subSections_), items = jumpGroups(SortBy::Title, itemSections_);
    for (auto& g : items)
        for (auto& e : g.entries) e.section += subSections_.size();
    if (!subs.empty() && !items.empty()) {
        for (auto& g : subs) g.label = g.label.empty() ? "Collections" : "Collections · " + g.label;
        for (auto& g : items) g.label = g.label.empty() ? "Items" : "Items · " + g.label;
    }
    subs.insert(subs.end(), items.begin(), items.end());
    return subs;
}

void CollectionScreen::onGesture(const Gesture& g, Nav& nav) {
    if (jump_) {
        Nav* n = &nav;
        return jumpSheetGesture(
            jump_, g, sheetArea(), nav, pager_.page(), pager_.pages(), [this](size_t s) { return pager_.pageOf(starts_.at(s)); },
            [this, n](int page) {
                if (pager_.goTo(page)) load(n->library());
                n->damage(contentArea(), Waveform::Quality);
            });
    }
    if (sheet_) {
        if (g.type != GestureType::Tap) return;
        sheet_->tap(g.pos, nav);
        sheet_.reset();
        load(nav.library());  // the kept mark
        loadKept(nav);
        nav.damage(contentArea(), Waveform::Quality);
        return;
    }
    if (g.type == GestureType::LongPress) {
        auto rects = lineRects();
        for (size_t i = 0; i < rects.size(); ++i)
            if (rects[i].contains(g.pos) && page_[i].isCollection) {
                const auto pins = nav.library().pinnedCollections();
                const bool pinned = std::find(pins.begin(), pins.end(), page_[i].collection.collection.key) != pins.end();
                sheet_.emplace(page_[i].collection, pinned);
                nav.damage(PinSheet::rect(), Waveform::Ui);
            }
        return;
    }
    if (g.type == GestureType::Tap && backRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (g.type == GestureType::Tap && searchRect().contains(g.pos)) {
        if (!group_) nav.push(std::make_unique<SearchScreen>(openItem_, status_, entry_.collection.key, entry_.collection.name));
        return;
    }
    if (g.type == GestureType::Tap && footerRect().contains(g.pos)) {
        if (pager_.pages() > 1) {
            jump_.emplace(jumpIndex(), pager_.page(), pager_.pages());
            nav.damage(sheetArea(), Waveform::Ui);
        }
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
    if (e.kind == SyncEvent::Kind::KeptChanged) {
        const auto was = kept_;
        loadKept(nav);
        auto rects = lineRects();
        for (size_t i = 0; i < page_.size(); ++i) {
            if (!page_[i].isCollection) continue;
            const std::string& key = page_[i].collection.collection.key;
            // The note's last change (gone) rests: the quality waveform, no ghost of "Downloading…".
            if (statusOf(was, key) != statusOf(kept_, key))
                nav.damage(rects[i], statusOf(kept_, key) ? Waveform::Ui : Waveform::Quality);
        }
        return;
    }
    if (e.kind == SyncEvent::Kind::DownloadProgress || e.kind == SyncEvent::Kind::DownloadFinished) {
        // Only the row of that file (kept collections download one after another): refreshed
        // from the library when it finished, not the whole page.
        auto rects = lineRects();
        for (size_t i = 0; i < page_.size(); ++i) {
            if (page_[i].isCollection || (page_[i].item.attachmentKey != e.attachmentKey && page_[i].item.key != e.itemKey)) continue;
            if (e.kind == SyncEvent::Kind::DownloadFinished)
                if (auto row = nav.library().rowFor(page_[i].item.key)) page_[i].item = std::move(*row);
            nav.damage(rects[i], Waveform::Ui);
        }
        return;
    }
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    load(nav.library());
    nav.damage(contentArea(), Waveform::Quality);
}

}  // namespace zk
