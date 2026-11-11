#include "screens/item_list.h"

#include "screens/item_details.h"

#include <algorithm>

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

void ItemList::setSource(Fetch f, Count c, Library& lib, Sections s) {
    fetch_ = std::move(f);
    count_ = std::move(c);
    sectionsOf_ = std::move(s);
    pager_ = Pager(0, perPage_);
    count(lib);
    load(lib);
}

void ItemList::refresh(Library& lib) {
    count(lib);
    load(lib);
}

void ItemList::count(Library& lib) {
    sections_ = sectionsOf_ ? sectionsOf_(lib) : std::vector<ListSection>{};
    sectionStarts_.clear();
    int64_t total = 0;
    for (const auto& s : sections_) {
        sectionStarts_.push_back(total);
        total += s.count;
    }
    if (sectionsOf_) pager_.setStarts(total, pageStarts(sections_, perPage_));
    else pager_.setTotal(count_ ? count_(lib) : 0);
}

std::vector<int64_t> ItemList::pageStarts(const std::vector<ListSection>& sections, int rowsPerPage) {
    const int room = std::max(3, 2 * rowsPerPage);  // in half rows: a heading takes 1, a row 2
    std::vector<int64_t> starts = {0};
    int used = 0;
    int64_t row = 0;
    for (const auto& s : sections) {
        if (s.count <= 0) continue;
        if (used > 0 && used + 3 > room) {  // the heading and a first row don't fit: a new page
            starts.push_back(row);
            used = 0;
        }
        used += 1;
        for (int64_t left = s.count; left > 0;) {
            int64_t fit = (room - used) / 2;
            if (fit <= 0) {
                starts.push_back(row);
                used = 0;
                fit = room / 2;
            }
            const int64_t take = std::min(fit, left);
            row += take;
            left -= take;
            used += static_cast<int>(2 * take);
        }
    }
    return starts;
}

bool ItemList::goToPage(int page, Library& lib) {
    if (!pager_.goTo(page)) return false;
    load(lib);
    return true;
}

int ItemList::sectionPage(size_t section) const {
    return section < sectionStarts_.size() ? pager_.pageOf(sectionStarts_[section]) : 0;
}

const ListSection* ItemList::sectionAt(int64_t row) const {
    auto it = std::lower_bound(sectionStarts_.begin(), sectionStarts_.end(), row);
    if (it == sectionStarts_.end() || *it != row) return nullptr;
    // Empty sections share a start with the next: the heading is the last one's.
    while (it + 1 != sectionStarts_.end() && *(it + 1) == row) ++it;
    return &sections_[static_cast<size_t>(it - sectionStarts_.begin())];
}

bool ItemList::startsSection(int64_t row) const { return sectionAt(row) != nullptr; }

void ItemList::load(Library& lib) {
    rows_ = fetch_ ? fetch_(lib, pager_.offset(), pager_.rowsOnPage()) : std::vector<ItemRow>{};
}

std::vector<Rect> ItemList::rowRects(Rect area) const {
    std::vector<Rect> out;
    int y = area.y;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (startsSection(pager_.offset() + static_cast<int64_t>(i))) y += kHeadingH;
        out.push_back({area.x + kMargin, y, area.w - 2 * kMargin, kRowH});
        y += kRowH;
    }
    return out;
}

std::vector<std::pair<std::string, Rect>> ItemList::headings(Rect area) const {
    std::vector<std::pair<std::string, Rect>> out;
    const auto rows = rowRects(area);
    for (size_t i = 0; i < rows.size(); ++i)
        if (const ListSection* s = sectionAt(pager_.offset() + static_cast<int64_t>(i)))
            out.emplace_back(s->label, Rect{rows[i].x, rows[i].y - kHeadingH, rows[i].w, kHeadingH});
    return out;
}

std::string ItemList::statusFor(const std::string& key) const { return statusFn_ && !key.empty() ? statusFn_(key) : std::string{}; }

void ItemList::onDownloadEvent(const SyncEvent& e, Rect area, Nav& nav) {
    if (e.kind != SyncEvent::Kind::DownloadProgress && e.kind != SyncEvent::Kind::DownloadFinished) return;
    // The row's file, or another of the item's (chosen on "Open which file?").
    int i = indexOfAttachment(e.attachmentKey);
    for (size_t r = 0; i < 0 && r < rows_.size(); ++r)
        if (!e.itemKey.empty() && rows_[r].key == e.itemKey) i = static_cast<int>(r);
    if (i < 0) return;
    if (e.kind == SyncEvent::Kind::DownloadFinished) {
        // The file state changed, and opening the book may have moved it (Unread, Recent...):
        // reload and repaint the page rather than trust the old row index.
        refresh(nav.library());
        nav.damage(area, Waveform::Quality);
        return;
    }
    nav.damage(rowRects(area)[static_cast<size_t>(i)], Waveform::Ui);
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
    for (const auto& [label, r] : headings(area)) drawSectionHeading(cr, r, label);
    drawPageIndicator(cr, {area.x, area.y + area.h - kPageIndicatorH, area.w, kPageIndicatorH}, pager_.page(),
                      pager_.pages());
    if (sheet_) sheet_->draw(cr, area);
}

bool ItemList::onGesture(const Gesture& g, Rect area, Nav& nav, const std::function<void(const ItemRow&)>& open) {
    if (sheet_) {
        switch (sheet_->onGesture(g, area, nav)) {
            case ItemSheet::Action::Close:
                sheet_.reset();
                nav.damage(area, Waveform::Ui);
                break;
            case ItemSheet::Action::Open: {
                const ItemRow row = sheet_->row();
                sheet_.reset();
                nav.damage(area, Waveform::Ui);
                if (open) open(row);
                break;
            }
            case ItemSheet::Action::Changed:
                load(nav.library());  // the row changed: in Zotero now, or read
                nav.damage(area, Waveform::Ui);
                break;
            case ItemSheet::Action::More: {
                const ItemRow row = sheet_->row();
                sheet_.reset();
                nav.damage(area, Waveform::Ui);
                nav.push(std::make_unique<ItemDetailsScreen>(row, open));
                break;
            }
            case ItemSheet::Action::None: break;
        }
        return true;  // while the sheet is up, the list underneath takes nothing
    }
    if (g.type == GestureType::LongPress) {
        auto rects = rowRects(area);
        for (size_t i = 0; i < rects.size(); ++i)
            if (rects[i].contains(g.pos)) {
                if (auto d = nav.library().details(rows_[i].key)) {
                    sheet_.emplace(rows_[i], std::move(*d));
                    nav.damage(area, Waveform::Ui);
                }
                return true;
            }
        return false;
    }
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
