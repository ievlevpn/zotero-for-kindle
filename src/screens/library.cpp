#include "screens/library.h"

#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236, kH = 1547;
constexpr int kChipsH = 80, kChipsGap = 10;
const SortBy kSorts[] = {SortBy::Recent, SortBy::Title, SortBy::Author, SortBy::Year};

}  // namespace

const char* sortLabel(SortBy s) {
    switch (s) {
        case SortBy::Title: return "Title";
        case SortBy::Author: return "Author";
        case SortBy::Year: return "Year";
        case SortBy::Recent: break;
    }
    return "Recent";
}

LibraryScreen::LibraryScreen(std::function<void(const ItemRow&)> openItem, StatusFn status) : openItem_(std::move(openItem)) {
    list_.setRowsPerPage(ItemList::rowsFitting(listArea()));
    list_.setStatusFn(std::move(status));
}

Rect LibraryScreen::chipsArea() { return {kMargin, kHeaderH, kW - 2 * kMargin, kChipsH}; }
Rect LibraryScreen::listArea() {
    const int y = kHeaderH + kChipsH + kChipsGap;  // 6 rows + page indicator fit below
    return {0, y, kW, kH - kTabBarH - y};
}
Rect LibraryScreen::sortButtonRect() { return {kW - kMargin - 260, 40, 260, 80}; }
Rect LibraryScreen::menuRect() const { return {kW - kMargin - 360, 124, 360, 4 * 100}; }

std::vector<std::string> LibraryScreen::chipLabels() const {
    std::vector<std::string> labels = {"All", "EPUB", "PDF", "Unread"};
    for (const auto& [tag, n] : tags_) labels.push_back(tag);
    return labels;
}

void LibraryScreen::onShow(Nav& nav) {
    tags_ = nav.library().topTags(3);
    if (!list_.pager().total() && list_.rows().empty()) apply(nav);
    else list_.refresh(nav.library());
}

void LibraryScreen::apply(Nav& nav) {
    query_.filter = Filter::All;
    query_.tag.clear();
    if (chip_ >= 1 && chip_ <= 3) query_.filter = chip_ == 1 ? Filter::Epub : chip_ == 2 ? Filter::Pdf : Filter::Unread;
    if (chip_ >= 4 && chip_ - 4 < static_cast<int>(tags_.size())) query_.tag = tags_[chip_ - 4].first;
    ItemList fresh = ItemList::forQuery(query_);
    fresh.setRowsPerPage(ItemList::rowsFitting(listArea()));
    ItemList::Fetch f = [q = query_](Library& lib, int64_t offset, int limit) {
        ListQuery page = q;
        page.offset = static_cast<int>(offset);
        page.limit = limit;
        return lib.rows(page);
    };
    list_.setSource(f, [q = query_](Library& lib) { return lib.countRows(q); }, nav.library());
}

void LibraryScreen::selectSort(SortBy s, Nav& nav) {
    query_.sort = s;
    menuOpen_ = false;
    apply(nav);
    nav.redrawAll(Waveform::Quality);
}

void LibraryScreen::selectChip(int index, Nav& nav) {
    chip_ = index;
    apply(nav);
    nav.damage({0, kHeaderH, kW, kH - kTabBarH - kHeaderH}, Waveform::Quality);
}

void LibraryScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    drawHeader(cr, "Library", false);
    const Rect sort = sortButtonRect();
    drawText(cr, std::string(sortLabel(query_.sort)) + " ▾", {kSans, 8.5, 500, false, kSecondary},
             {sort.x, sort.y + 20, sort.w, 60}, 1, Align::Right);
    drawChips(cr, chipsArea(), chipLabels(), chip_);
    list_.draw(cr, listArea());
    if (list_.rows().empty())
        drawText(cr, "Nothing here yet.", {kSerif, 11, 400, false, kSecondary}, {kMargin, listArea().y + 200, kW - 2 * kMargin, 100},
                 1, Align::Center);
    if (menuOpen_) {
        const Rect m = menuRect();
        fillRect(cr, m, 255);
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_rectangle(cr, m.x + 1.5, m.y + 1.5, m.w - 3, m.h - 3);
        cairo_stroke(cr);
        for (int i = 0; i < 4; ++i) {
            const Rect r{m.x, m.y + i * 100, m.w, 100};
            const bool on = kSorts[i] == query_.sort;
            centredText(cr, std::string(on ? "✓ " : "") + sortLabel(kSorts[i]), {kSans, 9, on ? 700 : 400},
                        {r.x + 40, r.y, r.w - 80, r.h}, Align::Left);
            if (i) hRule(cr, r.y, r.x + 20, r.x + r.w - 20);
        }
    }
}

void LibraryScreen::onGesture(const Gesture& g, Nav& nav) {
    if (menuOpen_) {
        if (g.type != GestureType::Tap) return;
        const Rect m = menuRect();
        menuOpen_ = false;
        if (m.contains(g.pos)) {
            selectSort(kSorts[(g.pos.y - m.y) / 100], nav);
            return;
        }
        nav.redrawAll(Waveform::Quality);  // closed without choosing
        return;
    }
    if (g.type == GestureType::Tap && sortButtonRect().contains(g.pos)) {
        menuOpen_ = true;
        nav.damage(menuRect(), Waveform::Ui);
        return;
    }
    if (g.type == GestureType::Tap && chipsArea().contains(g.pos)) {
        auto rects = chipRects(chipsArea(), chipLabels());
        for (size_t i = 0; i < rects.size(); ++i)
            if (rects[i].contains(g.pos)) selectChip(static_cast<int>(i), nav);
        return;
    }
    list_.onGesture(g, listArea(), nav, openItem_);
}

void LibraryScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    list_.onDownloadEvent(e, listArea(), nav);
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    tags_ = nav.library().topTags(3);
    list_.refresh(nav.library());
    nav.damage(listArea(), Waveform::Quality);
}

}  // namespace zk
