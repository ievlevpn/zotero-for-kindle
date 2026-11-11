#include "screens/library_sheets.h"

#include <algorithm>
#include <climits>

#include <glib.h>

#include <nlohmann/json.hpp>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;
using nlohmann::json;

namespace {

constexpr int kPad = 40, kTitleH = 90, kGroupH = 170, kGroupLabelH = 56, kChipH = 80, kButtonH = 100, kButtonW = 260;
constexpr int kOptionH = 100;
constexpr int kCellH = 100, kCellMinH = 80, kCellGap = 14, kCellMinW = 110, kJumpLabelH = 56, kGroupGap = 16, kSheetPagerH = 56;
const TextStyle kSheetTitle{kSans, 11, 700}, kGroupLabel{kSans, 7.5, 400, false, kSecondary}, kCell{kSans, 9.5, 700};

const SortBy kSorts[] = {SortBy::Recent, SortBy::Added, SortBy::Title, SortBy::Author, SortBy::Year};
const TypeFilter kTypes[] = {TypeFilter::Any, TypeFilter::WithFile, TypeFilter::Pdf, TypeFilter::Epub};
const ReadFilter kReads[] = {ReadFilter::Any, ReadFilter::Unread, ReadFilter::Read};
const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

const char* typeName(TypeFilter t) {
    switch (t) {
        case TypeFilter::Any: return "any";
        case TypeFilter::Pdf: return "pdf";
        case TypeFilter::Epub: return "epub";
        case TypeFilter::WithFile: break;
    }
    return "file";
}

const char* readName(ReadFilter r) { return r == ReadFilter::Unread ? "unread" : r == ReadFilter::Read ? "read" : "any"; }

const char* sortName(SortBy s) {
    switch (s) {
        case SortBy::Title: return "title";
        case SortBy::Author: return "author";
        case SortBy::Year: return "year";
        case SortBy::Added: return "added";
        case SortBy::Recent: break;
    }
    return "recent";
}

template <class E, size_t N> E fromName(const E (&all)[N], const char* (*name)(E), const std::string& s, E fallback) {
    for (E e : all)
        if (s == name(e)) return e;
    return fallback;
}

void drawSheet(cairo_t* cr, Rect area, Rect s, const std::string& title) {
    cairo_save(cr);  // fade what's behind, so the sheet reads as on top
    cairo_rectangle(cr, area.x, area.y, area.w, area.h);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
    cairo_fill(cr);
    cairo_restore(cr);
    fillRect(cr, s, 255);
    fillRect(cr, {s.x, s.y, s.w, 4}, kInk);
    drawText(cr, title, kSheetTitle, {s.x + kMargin, s.y + kPad, s.w - 2 * kMargin, kTitleH - 10}, 1);
}

int monthOf(const std::string& key) {  // "2025-03" -> 3; 0 if not a month
    if (key.size() < 7 || key[4] != '-') return 0;
    const int m = std::atoi(key.c_str() + 5);
    return m >= 1 && m <= 12 ? m : 0;
}

std::string shortened(const std::string& s, size_t max) {
    if (g_utf8_strlen(s.c_str(), -1) <= static_cast<glong>(max)) return s;
    const char* end = g_utf8_offset_to_pointer(s.c_str(), static_cast<glong>(max - 1));
    return std::string(s.c_str(), end) + "…";
}

}  // namespace

// ---- the view ------------------------------------------------------------------------------

bool LibraryView::filtered() const { return type != TypeFilter::WithFile || read != ReadFilter::Any || !tag.empty(); }

LibraryView LibraryView::cleared() const {
    LibraryView v = *this;
    v.type = TypeFilter::WithFile;
    v.read = ReadFilter::Any;
    v.tag.clear();
    return v;
}

ListQuery LibraryView::query() const {
    ListQuery q;
    q.sort = sort;
    q.type = type;
    q.read = read;
    q.downloaded = downloaded;
    q.tag = tag;
    return q;
}

LibraryView loadLibraryView(Library& lib) {
    LibraryView v;
    const json j = json::parse(lib.metaValue("libraryView"), nullptr, false);
    if (!j.is_object()) return v;
    v.downloaded = j.value("downloaded", false);
    v.type = fromName(kTypes, typeName, j.value("type", ""), v.type);
    v.read = fromName(kReads, readName, j.value("read", ""), v.read);
    v.tag = j.value("tag", "");
    v.sort = fromName(kSorts, sortName, j.value("sort", ""), v.sort);
    return v;
}

void saveLibraryView(Library& lib, const LibraryView& v) {
    lib.setMetaValue("libraryView", json{{"downloaded", v.downloaded},
                                         {"type", typeName(v.type)},
                                         {"read", readName(v.read)},
                                         {"tag", v.tag},
                                         {"sort", sortName(v.sort)}}
                                        .dump());
}

std::string withThousands(int64_t n) {
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return n < 0 ? "-" + out : out;
}

std::string filterLine(const LibraryView& v, int64_t count) {
    std::string s;
    auto add = [&](const std::string& part) { s += part + " · "; };
    switch (v.type) {
        case TypeFilter::Any: add("All types"); break;
        case TypeFilter::Pdf: add("PDF"); break;
        case TypeFilter::Epub: add("EPUB"); break;
        case TypeFilter::WithFile: break;
    }
    if (v.read == ReadFilter::Unread) add("Unread");
    if (v.read == ReadFilter::Read) add("Read");
    if (!v.tag.empty()) add("tag: " + v.tag);
    return s + withThousands(count) + (count == 1 ? " item" : " items");
}

const char* sortLabel(SortBy s) {
    switch (s) {
        case SortBy::Title: return "Title";
        case SortBy::Author: return "Author";
        case SortBy::Year: return "Year";
        case SortBy::Added: return "Date added";
        case SortBy::Recent: break;
    }
    return "Recent";
}

std::string sectionLabel(SortBy sort, const std::string& key) {
    switch (sort) {
        case SortBy::Title:
        case SortBy::Author: return key.size() > 1 ? key.substr(1) : "#";
        case SortBy::Year: return key.empty() ? "No year" : key;
        case SortBy::Added: {
            const int m = monthOf(key);
            return m ? std::string(kMonths[m - 1]) + " " + key.substr(0, 4) : "No date";
        }
        case SortBy::Recent: break;
    }
    return key;
}

std::vector<JumpGroup> jumpGroups(SortBy sort, const std::vector<ListSection>& sections) {
    std::vector<JumpGroup> out;
    auto into = [&](const std::string& group, const std::string& label, size_t i) {
        if (out.empty() || out.back().label != group) out.push_back({group, {}});
        out.back().entries.push_back({label, i});
    };
    for (size_t i = 0; i < sections.size(); ++i) {
        const std::string& key = sections[i].key;
        switch (sort) {
            case SortBy::Title:
            case SortBy::Author:
                into(key.empty() || key[0] == '3' ? "" : key[0] == '1' ? "A–Z" : "А–Я", sectionLabel(sort, key), i);
                break;
            case SortBy::Year: into("", sectionLabel(sort, key), i); break;
            case SortBy::Added: {
                const int m = monthOf(key);
                if (m) into(key.substr(0, 4), std::string(kMonths[m - 1]).substr(0, 3), i);
                else into("", "No date", i);
                break;
            }
            case SortBy::Recent: return {};
        }
    }
    return out;
}

Rect sheetRectIn(Rect area, int height) {
    const int h = std::min(height, area.h);
    return {area.x, area.y + area.h - h, area.w, h};
}

// ---- Filter --------------------------------------------------------------------------------

Rect FilterSheet::sheetRect(Rect area) const {
    return sheetRectIn(area, kPad + kTitleH + 3 * kGroupH + 20 + kButtonH + kPad);
}

Rect FilterSheet::chipsArea(Rect area, int group) const {
    const Rect s = sheetRect(area);
    return {s.x + kMargin, s.y + kPad + kTitleH + group * kGroupH + kGroupLabelH, s.w - 2 * kMargin, kChipH};
}

std::vector<std::string> FilterSheet::tagLabels() const {
    return {"None", draft_.tag.empty() ? "Choose a tag…" : shortened(draft_.tag, 28)};
}

std::vector<Rect> FilterSheet::typeRects(Rect area) const {
    return chipRects(chipsArea(area, 0), {"Any", "With a file", "PDF", "EPUB"});
}
std::vector<Rect> FilterSheet::readRects(Rect area) const { return chipRects(chipsArea(area, 1), {"Any", "Unread", "Read"}); }
std::vector<Rect> FilterSheet::tagRects(Rect area) const { return chipRects(chipsArea(area, 2), tagLabels()); }

Rect FilterSheet::resetRect(Rect area) const {
    const Rect s = sheetRect(area);
    return {s.x + kMargin, s.y + s.h - kPad - kButtonH, kButtonW, kButtonH};
}

Rect FilterSheet::doneRect(Rect area) const {
    const Rect s = sheetRect(area);
    return {s.x + s.w - kMargin - kButtonW, s.y + s.h - kPad - kButtonH, kButtonW, kButtonH};
}

void FilterSheet::draw(cairo_t* cr, Rect area) const {
    const Rect s = sheetRect(area);
    drawSheet(cr, area, s, "Filter");
    const char* names[] = {"Type", "Read", "Tag"};
    for (int g = 0; g < 3; ++g) {
        const Rect chips = chipsArea(area, g);
        drawText(cr, names[g], kGroupLabel, {chips.x, chips.y - kGroupLabelH, chips.w, kGroupLabelH - 6}, 1);
    }
    const int type = static_cast<int>(std::find(std::begin(kTypes), std::end(kTypes), draft_.type) - std::begin(kTypes));
    const int read = static_cast<int>(std::find(std::begin(kReads), std::end(kReads), draft_.read) - std::begin(kReads));
    drawChips(cr, chipsArea(area, 0), {"Any", "With a file", "PDF", "EPUB"}, type);
    drawChips(cr, chipsArea(area, 1), {"Any", "Unread", "Read"}, read);
    drawChips(cr, chipsArea(area, 2), tagLabels(), draft_.tag.empty() ? 0 : 1);
    drawButton(cr, resetRect(area), "Reset", false);
    drawButton(cr, doneRect(area), "Done", true);
}

FilterSheet::Action FilterSheet::onGesture(const Gesture& g, Rect area) {
    if (g.type != GestureType::Tap) return Action::None;
    if (!sheetRect(area).contains(g.pos)) return Action::Close;
    auto hit = [&](const std::vector<Rect>& rects) {
        for (size_t i = 0; i < rects.size(); ++i)
            if (rects[i].contains(g.pos)) return static_cast<int>(i);
        return -1;
    };
    if (int i = hit(typeRects(area)); i >= 0) {
        draft_.type = kTypes[i];
        return Action::Changed;
    }
    if (int i = hit(readRects(area)); i >= 0) {
        draft_.read = kReads[i];
        return Action::Changed;
    }
    if (int i = hit(tagRects(area)); i >= 0) {
        if (i == 1) return Action::PickTag;
        draft_.tag.clear();
        return Action::Changed;
    }
    if (resetRect(area).contains(g.pos)) {
        draft_ = draft_.cleared();
        return Action::Changed;
    }
    if (doneRect(area).contains(g.pos)) return Action::Apply;
    return Action::None;
}

// ---- Sort ----------------------------------------------------------------------------------

Rect SortSheet::sheetRect(Rect area) const { return sheetRectIn(area, kPad + kTitleH + 5 * kOptionH + kPad); }

std::vector<Rect> SortSheet::optionRects(Rect area) const {
    const Rect s = sheetRect(area);
    std::vector<Rect> out;
    for (int i = 0; i < 5; ++i) out.push_back({s.x + kMargin, s.y + kPad + kTitleH + i * kOptionH, s.w - 2 * kMargin, kOptionH});
    return out;
}

void SortSheet::draw(cairo_t* cr, Rect area) const {
    drawSheet(cr, area, sheetRect(area), "Sort by");
    const auto rects = optionRects(area);
    for (size_t i = 0; i < rects.size(); ++i) {
        const bool on = kSorts[i] == current_;
        centredText(cr, sortLabel(kSorts[i]), {kSans, 9, on ? 700 : 400}, {rects[i].x, rects[i].y, rects[i].w - 80, rects[i].h},
                    Align::Left);
        if (on) centredText(cr, "✓", {kSans, 9, 700}, {rects[i].x + rects[i].w - 80, rects[i].y, 80, rects[i].h}, Align::Right);
        if (i) hRule(cr, rects[i].y, rects[i].x, rects[i].x + rects[i].w);
    }
}

SortSheet::Action SortSheet::onGesture(const Gesture& g, Rect area) {
    if (g.type != GestureType::Tap) return Action::None;
    if (!sheetRect(area).contains(g.pos)) return Action::Close;
    const auto rects = optionRects(area);
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) {
            current_ = kSorts[i];
            return Action::Chosen;
        }
    return Action::None;
}

// ---- Jump ----------------------------------------------------------------------------------

namespace {
bool placeholder(const std::string& label) { return label.rfind("No ", 0) == 0; }  // "No year", "No date"
}  // namespace

// The cells' size: as large as they can be, smaller (to about 7 mm) rather than a second sheet
// page. A cell is as wide as the widest label but the placeholders, which are wider on their own.
JumpSheet::Grid JumpSheet::grid(Rect area) const {
    const int contentW = area.w - 2 * kMargin, room = area.h - (kPad + kTitleH + kPad);
    int label = 0;
    for (const auto& g : groups_)
        for (const auto& e : g.entries)
            if (!placeholder(e.label)) label = std::max(label, measureText(e.label, kCell, contentW, 1).width);
    Grid gr{std::max(kCellMinW, label + 50), kCellH, contentW};
    while (contentHeight(gr) > room && gr.cellH > kCellMinH) {
        gr.cellH -= 4;
        gr.cellW = std::max(label + 30, gr.cellW - 4);
    }
    return gr;
}

int JumpSheet::cellWidth(const Grid& gr, const std::string& label) const {
    if (!placeholder(label)) return gr.cellW;
    return std::min(gr.width, std::max(gr.cellW, measureText(label, kCell, gr.width, 1).width + 40));
}

// Lays the groups out top to bottom, cells left to right; place(groupLabel?, x, y, w, h, entry).
template <class F> int JumpSheet::flow(const Grid& gr, F&& place) const {
    int y = 0;
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        if (gi) y += kGroupGap;
        if (!g.label.empty()) {
            place(true, 0, y, gr.width, kJumpLabelH, nullptr);
            y += kJumpLabelH;
        }
        int x = 0;
        for (const auto& e : g.entries) {
            const int w = cellWidth(gr, e.label);
            if (x > 0 && x + w > gr.width) {
                x = 0;
                y += gr.cellH + kCellGap;
            }
            place(false, x, y, w, gr.cellH, &e);
            x += w + kCellGap;
        }
        if (!g.entries.empty()) y += gr.cellH + kCellGap;
    }
    return y;
}

int JumpSheet::contentHeight(const Grid& gr) const {
    return flow(gr, [](bool, int, int, int, int, const JumpEntry*) {});
}

int JumpSheet::contentHeight(Rect area) const { return groups_.empty() ? kButtonH + 30 + 120 : contentHeight(grid(area)); }

// Cells and group labels in reading order, broken into the sheet's pages where they don't fit:
// a whole row of cells moves on (a group label with it).
std::vector<JumpSheet::Placed> JumpSheet::layout(Rect area) const {
    std::vector<Placed> out;
    if (groups_.empty()) return out;
    const Grid gr = grid(area);
    size_t group = 0;
    const int height = flow(gr, [&](bool isLabel, int x, int y, int w, int h, const JumpEntry* e) {
        if (isLabel)
            while (groups_[group].label.empty()) ++group;
        out.push_back({0, {x, y, w, h}, isLabel ? groups_[group++].label : e->label, isLabel, e ? e->section : 0});
    });
    const int room = area.h - (kPad + kTitleH + kPad) - kSheetPagerH;
    if (height <= room + kSheetPagerH) return out;
    // A row reaching below the room starts the next page, a group label just above it with it.
    int page = 0, shift = 0;
    for (Placed& p : out) {
        const int bottom = p.rect.y - shift + p.rect.h + (p.isGroupLabel ? gr.cellH : 0);
        if (p.rect.y - shift > 0 && bottom > room && (p.isGroupLabel || p.rect.x == 0)) {
            ++page;
            shift = p.rect.y;
        }
        p.sheetPage = page;
        p.rect.y -= shift;
    }
    return out;
}

int JumpSheet::sheetPages(Rect area) const {
    int pages = 1;
    for (const auto& p : layout(area)) pages = std::max(pages, p.sheetPage + 1);
    return pages;
}

Rect JumpSheet::sheetRect(Rect area) const { return sheetRectIn(area, kPad + kTitleH + contentHeight(area) + kPad); }

std::vector<std::pair<std::string, Rect>> JumpSheet::cells(Rect area) const {
    const Rect s = sheetRect(area);
    std::vector<std::pair<std::string, Rect>> out;
    for (const auto& p : layout(area))
        if (p.sheetPage == sheetPage_ && !p.isGroupLabel)
            out.emplace_back(p.label, Rect{s.x + kMargin + p.rect.x, s.y + kPad + kTitleH + p.rect.y, p.rect.w, p.rect.h});
    return out;
}

Rect JumpSheet::firstPageRect(Rect area) const {
    const Rect s = sheetRect(area);
    return {s.x + kMargin, s.y + kPad + kTitleH, s.w - 2 * kMargin, kButtonH};
}

Rect JumpSheet::pageFieldRect(Rect area) const {
    const Rect f = firstPageRect(area);
    return {f.x, f.y + f.h + 30, f.w, 120};
}

void JumpSheet::draw(cairo_t* cr, Rect area) const {
    const Rect s = sheetRect(area);
    drawSheet(cr, area, s, "Go to");
    if (groups_.empty()) {
        drawButton(cr, firstPageRect(area), "First page", false);
        const Rect f = pageFieldRect(area);
        roundedRect(cr, f.x + 1.5, f.y + 1.5, f.w - 3, f.h - 3, 12);
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        centredText(cr, "Page " + withThousands(page_ + 1) + " of " + withThousands(pages_), {kSans, 10, 400},
                    {f.x + 30, f.y, f.w - 60, f.h}, Align::Left);
        centredText(cr, "Go to page…", {kSans, 8, 700, false, kSecondary}, {f.x + 30, f.y, f.w - 60, f.h}, Align::Right);
        return;
    }
    for (const auto& p : layout(area)) {
        if (p.sheetPage != sheetPage_) continue;
        const Rect r{s.x + kMargin + p.rect.x, s.y + kPad + kTitleH + p.rect.y, p.rect.w, p.rect.h};
        if (p.isGroupLabel) {
            drawText(cr, p.label, kGroupLabel, {r.x, r.y + 8, r.w, r.h - 8}, 1);
            continue;
        }
        gray(cr, 170);
        cairo_set_line_width(cr, 3);
        roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 14);
        cairo_stroke(cr);
        centredText(cr, p.label, kCell, r);
    }
    if (const int pages = sheetPages(area); pages > 1)
        drawPageIndicator(cr, {s.x, s.y + s.h - kPad - kSheetPagerH, s.w, kSheetPagerH}, sheetPage_, pages);
}

JumpSheet::Action JumpSheet::onGesture(const Gesture& g, Rect area) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up || g.dir == Direction::Down)) {
        const int to = sheetPage_ + (g.dir == Direction::Up ? 1 : -1);
        if (to < 0 || to >= sheetPages(area)) return Action::None;
        sheetPage_ = to;
        return Action::Changed;
    }
    if (g.type != GestureType::Tap) return Action::None;
    if (!sheetRect(area).contains(g.pos)) return Action::Close;
    if (groups_.empty()) {
        if (firstPageRect(area).contains(g.pos)) return Action::FirstPage;
        if (pageFieldRect(area).contains(g.pos)) return Action::GoToPage;
        return Action::None;
    }
    const Rect s = sheetRect(area);
    for (const auto& p : layout(area)) {
        const Rect r{s.x + kMargin + p.rect.x, s.y + kPad + kTitleH + p.rect.y, p.rect.w, p.rect.h};
        if (p.sheetPage == sheetPage_ && !p.isGroupLabel && r.contains(g.pos)) {
            target_ = p.section;
            return Action::Jump;
        }
    }
    return Action::None;
}

}  // namespace zk
