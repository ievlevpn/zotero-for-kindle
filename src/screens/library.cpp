#include "screens/library.h"

#include "screens/goto_page.h"
#include "screens/read_tag.h"
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
// The title row, the toggle under it, the filter line: 6 rows still fit on the PW5.
constexpr int kTitleRowH = 120, kSegmentH = 64, kSegmentW = 520, kLineY = kTitleRowH + kSegmentH + 4, kLineH = 52;
constexpr int kListY = kLineY + kLineH, kHeaderButtonW = 200, kClearW = 160;
const TextStyle kLine{kSans, 7.5, 400, false, kSecondary};

}  // namespace

LibraryScreen::LibraryScreen(std::function<void(const ItemRow&)> openItem, StatusFn status) : openItem_(std::move(openItem)) {
    list_.setRowsPerPage(ItemList::rowsFitting(listArea()));
    list_.setStatusFn(std::move(status));
}

Rect LibraryScreen::sortButtonRect() { return {W() - kMargin - kHeaderButtonW, 22, kHeaderButtonW, 80}; }
Rect LibraryScreen::filterButtonRect() { return {sortButtonRect().x - 20 - kHeaderButtonW, 22, kHeaderButtonW, 80}; }
Rect LibraryScreen::segmentRect(int index) {
    return segmentRects({kMargin, kTitleRowH, kSegmentW, kSegmentH}, 2)[static_cast<size_t>(index == 1)];
}
Rect LibraryScreen::filterLineRect() { return {kMargin, kLineY, W() - 2 * kMargin, kLineH}; }
Rect LibraryScreen::clearRect() { return {W() - kMargin - kClearW, kLineY, kClearW, kLineH}; }
Rect LibraryScreen::listArea() { return {0, kListY, W(), H() - kTabBarH - kListY}; }
Rect LibraryScreen::footerRect() {
    const Rect a = listArea();
    return {0, a.y + a.h - kPageIndicatorH, W(), kPageIndicatorH};
}
Rect LibraryScreen::sheetArea() { return {0, kTitleRowH, W(), H() - kTabBarH - kTitleRowH}; }

std::string LibraryScreen::filterLine() const { return zk::filterLine(view_, list_.pager().total()); }

void LibraryScreen::onShow(Nav& nav) {
    if (!loaded_) {
        view_ = loadLibraryView(nav.library());
        loaded_ = true;
        apply(nav);
        return;
    }
    list_.refresh(nav.library());
}

void LibraryScreen::apply(Nav& nav) {
    const ListQuery q = view_.query();
    ItemList::Fetch f = [q](Library& lib, int64_t offset, int limit) {
        ListQuery page = q;
        page.offset = static_cast<int>(offset);
        page.limit = limit;
        return lib.rows(page);
    };
    ItemList::Sections sections;
    if (q.sort != SortBy::Recent)
        sections = [q](Library& lib) {
            std::vector<ListSection> out;
            for (const auto& s : lib.sectionCounts(q)) out.push_back({s.key, sectionLabel(q.sort, s.key), s.count});
            return out;
        };
    list_.setSource(f, [q](Library& lib) { return lib.countRows(q); }, nav.library(), sections);
}

void LibraryScreen::setView(const LibraryView& v, Nav& nav) {
    closeSheets();
    view_ = v;
    saveLibraryView(nav.library(), view_);
    apply(nav);
    changed(nav);
}

void LibraryScreen::changed(Nav& nav) { nav.damage(sheetArea(), Waveform::Quality); }

void LibraryScreen::closeSheets() {
    filter_.reset();
    sort_.reset();
    jump_.reset();
}

void LibraryScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Library", false, 2 * kHeaderButtonW + 40);
    drawButton(cr, filterButtonRect(), "Filter", false);
    drawButton(cr, sortButtonRect(), "Sort", false);
    drawSegments(cr, {kMargin, kTitleRowH, kSegmentW, kSegmentH}, {"All", "Downloaded"}, view_.downloaded ? 1 : 0);
    const Rect line = filterLineRect();
    const bool clear = view_.filtered();
    centredText(cr, filterLine(), kLine, {line.x, line.y, line.w - (clear ? kClearW + 20 : 0), line.h}, Align::Left);
    if (clear) centredText(cr, "Clear", {kSans, 7.5, 700, false, kInk}, clearRect(), Align::Right);
    list_.draw(cr, listArea());
    drawJumpIndicator(cr, footerRect(), list_.pager().page(), list_.pager().pages());  // it opens the jump sheet
    if (list_.rows().empty())
        drawText(cr,
                 view_.downloaded   ? "Nothing on this Kindle yet."
                 : view_.filtered() ? "No items match the filter."
                                    : "Nothing here yet.",
                 {kSerif, 11, 400, false, kSecondary}, {kMargin, listArea().y + 200, W() - 2 * kMargin, 100}, 1, Align::Center);
    if (filter_) filter_->draw(cr, sheetArea());
    if (sort_) sort_->draw(cr, sheetArea());
    if (jump_) jump_->draw(cr, sheetArea());
}

void LibraryScreen::onFilterGesture(const Gesture& g, Nav& nav) {
    switch (filter_->onGesture(g, sheetArea())) {
        case FilterSheet::Action::Close:
            filter_.reset();
            nav.damage(sheetArea(), Waveform::Ui);
            break;
        case FilterSheet::Action::Changed: nav.damage(filter_->sheetRect(sheetArea()), Waveform::Ui); break;
        case FilterSheet::Action::PickTag:
            nav.push(std::make_unique<TagPickerScreen>([this](const std::string& tag) {
                if (filter_) filter_->setTag(tag);
            }));
            break;
        case FilterSheet::Action::Apply: {
            const LibraryView v = filter_->draft();
            filter_.reset();
            setView(v, nav);
            break;
        }
        case FilterSheet::Action::None: break;
    }
}

void LibraryScreen::onSortGesture(const Gesture& g, Nav& nav) {
    switch (sort_->onGesture(g, sheetArea())) {
        case SortSheet::Action::Close:
            sort_.reset();
            nav.damage(sheetArea(), Waveform::Ui);
            break;
        case SortSheet::Action::Chosen: {
            LibraryView v = view_;
            v.sort = sort_->chosen();
            sort_.reset();
            setView(v, nav);
            break;
        }
        case SortSheet::Action::None: break;
    }
}

void LibraryScreen::openJump(Nav& nav) {
    jump_.emplace(jumpGroups(view_.sort, list_.sections()), list_.pager().page(), list_.pager().pages());
    nav.damage(sheetArea(), Waveform::Ui);
}

void LibraryScreen::onJumpGesture(const Gesture& g, Nav& nav) {
    switch (jump_->onGesture(g, sheetArea())) {
        case JumpSheet::Action::Close:
            jump_.reset();
            nav.damage(sheetArea(), Waveform::Ui);
            break;
        case JumpSheet::Action::Changed: nav.damage(jump_->sheetRect(sheetArea()), Waveform::Ui); break;
        case JumpSheet::Action::Jump: {
            const size_t section = jump_->target();
            jump_.reset();
            list_.goToPage(list_.sectionPage(section), nav.library());
            nav.damage(sheetArea(), Waveform::Quality);
            break;
        }
        case JumpSheet::Action::FirstPage:
            jump_.reset();
            list_.goToPage(0, nav.library());
            nav.damage(sheetArea(), Waveform::Quality);
            break;
        case JumpSheet::Action::GoToPage: {
            jump_.reset();
            Nav* n = &nav;
            nav.push(std::make_unique<GoToPageScreen>(
                list_.pager().pages(), list_.pager().page(),
                [this, n](int page) {
                    list_.goToPage(page, n->library());
                    n->damage(sheetArea(), Waveform::Quality);
                },
                false));
            break;
        }
        case JumpSheet::Action::None: break;
    }
}

void LibraryScreen::onGesture(const Gesture& g, Nav& nav) {
    if (filter_) return onFilterGesture(g, nav);
    if (sort_) return onSortGesture(g, nav);
    if (jump_) return onJumpGesture(g, nav);
    if (list_.sheet()) {  // an item's sheet: the list takes everything until it closes
        list_.onGesture(g, listArea(), nav, openItem_);
        return;
    }
    if (g.type == GestureType::Tap) {
        if (filterButtonRect().contains(g.pos)) {
            filter_.emplace(view_);
            nav.damage(sheetArea(), Waveform::Ui);
            return;
        }
        if (sortButtonRect().contains(g.pos)) {
            sort_.emplace(view_.sort);
            nav.damage(sheetArea(), Waveform::Ui);
            return;
        }
        for (int i = 0; i < 2; ++i)
            if (segmentRect(i).contains(g.pos)) {
                if ((i == 1) != view_.downloaded) {
                    LibraryView v = view_;
                    v.downloaded = i == 1;
                    setView(v, nav);
                }
                return;
            }
        if (view_.filtered() && clearRect().contains(g.pos)) return setView(view_.cleared(), nav);
        if (footerRect().contains(g.pos)) {
            if (list_.pager().pages() > 1) openJump(nav);
            return;
        }
    }
    list_.onGesture(g, listArea(), nav, openItem_);
}

void LibraryScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    list_.onDownloadEvent(e, listArea(), nav);
    if (e.kind == SyncEvent::Kind::DownloadFinished) nav.damage(filterLineRect(), Waveform::Ui);  // Downloaded's count
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    list_.refresh(nav.library());
    nav.damage({0, kLineY, W(), H() - kTabBarH - kLineY}, Waveform::Quality);
}

}  // namespace zk
