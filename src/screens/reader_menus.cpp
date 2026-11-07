#include "screens/reader_menus.h"

#include <algorithm>
#include <cmath>

#include "app/settings_store.h"
#include "docs/fonts.h"
#include "platform/lipc.h"
#include "screens/book_notes.h"
#include "screens/book_search.h"
#include "screens/contents.h"
#include "screens/goto_page.h"
#include "screens/reader.h"
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
constexpr int kTopH = 140, kPanelH = 380, kLightH = 170;
const char* kButtons[] = {"Contents", "Notes", "Display", "Light", "Sync"};
constexpr int kMaxLight = 24;  // com.lab126.powerd flIntensity range on the PW5
constexpr int kDisplayH = 580;
const char* kMarginNames[] = {"Narrow", "Normal", "Wide", "Extra wide"};
const int kSpacings[] = {100, 120, 140};
constexpr double kMinSize = 7, kMaxSize = 16, kSizeStep = 0.5;

}  // namespace

Rect ReaderMenus::topBarRect() { return {0, 0, W(), kTopH}; }
Rect ReaderMenus::panelRect() { return {0, H() - kPanelH, W(), kPanelH}; }
Rect ReaderMenus::backRect() { return {0, 0, 320, kTopH}; }
Rect ReaderMenus::searchRect() { return {W() - kMargin - 190, 30, 80, 80}; }
Rect ReaderMenus::contentsIconRect() { return {W() - kMargin - 90, 30, 90, 80}; }
Rect ReaderMenus::sliderRect() { return {kMargin, H() - kPanelH + 100, W() - 2 * kMargin, 70}; }
Rect ReaderMenus::buttonRect(int i) {
    const int w = (W() - 2 * kMargin) / 5;
    return {kMargin + i * w, H() - kPanelH + 210, w - 12, 120};
}
Rect ReaderMenus::lightPanelRect() { return {0, H() - kPanelH - kLightH, W(), kLightH}; }
Rect ReaderMenus::lightSliderRect() { return {kMargin + 120, H() - kPanelH - kLightH + 50, W() - 2 * kMargin - 240, 70}; }

Rect ReaderMenus::displayPanelRect() { return {0, H() - kPanelH - kDisplayH, W(), kDisplayH}; }
Rect ReaderMenus::fontRowRect() { return {kMargin, H() - kPanelH - kDisplayH + 24, W() - 2 * kMargin, 90}; }
Rect ReaderMenus::sizeMinusRect() { return {kMargin + 240, H() - kPanelH - kDisplayH + 134, 160, 90}; }
Rect ReaderMenus::sizePlusRect() { return {W() - kMargin - 160, H() - kPanelH - kDisplayH + 134, 160, 90}; }
Rect ReaderMenus::marginRect(int i) {
    const int w = (W() - 2 * kMargin - 240) / 4;
    return {kMargin + 240 + i * w, H() - kPanelH - kDisplayH + 244, w - 12, 90};
}
Rect ReaderMenus::pdfViewRect(int i) {
    const int w = (W() - 2 * kMargin - 240) / 3;
    return {kMargin + 240 + i * w, H() - kPanelH - kDisplayH + 244, w - 12, 90};
}
Rect ReaderMenus::orientationRect(int i) {
    const int w = (W() - 2 * kMargin - 240) / 3;
    return {kMargin + 240 + i * w, H() - kPanelH - kDisplayH + 464, w - 12, 90};
}
Rect ReaderMenus::spacingRect(int i) {
    const int w = (W() - 2 * kMargin - 240) / 3;
    return {kMargin + 240 + i * w, H() - kPanelH - kDisplayH + 354, w - 12, 90};
}

bool ReaderMenus::showsTextOptions() const { return reader_.type() != DocType::Pdf; }
bool ReaderMenus::showsPenOptions() const { return reader_.type() == DocType::Pdf && reader_.penSeen(); }
Rect ReaderMenus::penRect(int i) {
    const int w = (W() - 2 * kMargin - 240) / 2;
    return {kMargin + 240 + i * w, H() - kPanelH - kDisplayH + 354, w - 12, 90};
}

namespace {
std::vector<std::string> g_testFonts;
}  // namespace

void ReaderMenus::setFontsForTests(std::vector<std::string> f) { g_testFonts = std::move(f); }

const std::vector<std::string>& ReaderMenus::fonts() {
    if (!g_testFonts.empty()) return g_testFonts;
    // Scanning fonts reads every file with FreeType: once per process, not per menu.
    static const std::vector<std::string> all = [] {
        auto f = readingFonts(readingFontDirs());
        f.erase(std::remove_if(f.begin(), f.end(), isFastFont), f.end());
        const auto found = readingFonts(readingFontDirs());
        for (const char* fast : kFastFonts)  // last, Serif, Sans, Mono
            if (std::find(found.begin(), found.end(), fast) != found.end()) f.push_back(fast);
        return f;
    }();
    return all;
}

// ---- The font pop-up --------------------------------------------------------------------------

namespace {
constexpr int kPickerTitleH = 120, kPickerRowH = 100, kPickerHeadingH = 70, kPickerFooterH = 110, kPickerGap = 60;
}  // namespace

Rect ReaderMenus::fontPickerRect() const {
    // Above the bottom panel, as tall as it needs (or may) be.
    const int w = std::min(W() - 2 * kMargin, 900);
    const int maxH = H() - kPanelH - 2 * kPickerGap;
    return {(W() - w) / 2, kPickerGap, w, maxH};
}

bool ReaderMenus::headingAbove(size_t i, size_t from) {
    const auto& f = fonts();
    return isFastFont(f[i]) && (i == from || i == 0 || !isFastFont(f[i - 1]));
}

std::vector<ReaderMenus::PickerPage> ReaderMenus::pickerPages() {
    const auto& f = fonts();
    auto height = [&](size_t i, size_t from) { return kPickerRowH + (headingAbove(i, from) ? kPickerHeadingH : 0); };
    int all = 0;
    for (size_t i = 0; i < f.size(); ++i) all += height(i, 0);
    const int room = fontPickerRect().h - kPickerTitleH - (all + kPickerTitleH + 30 <= fontPickerRect().h ? 0 : kPickerFooterH);
    // The Fast group (heading and fonts) starts a page of its own if it doesn't fit where it begins
    // but would on a page.
    int fastH = 0;
    for (size_t i = 0; i < f.size(); ++i)
        if (isFastFont(f[i])) fastH += height(i, 0);
    std::vector<PickerPage> pages;
    for (size_t i = 0; i < f.size();) {
        PickerPage p{i, i};
        for (int used = 0; p.to < f.size() && (p.to == p.from || used + height(p.to, p.from) <= room); ++p.to) {
            const bool groupStarts = isFastFont(f[p.to]) && (p.to == 0 || !isFastFont(f[p.to - 1]));
            if (groupStarts && p.to > p.from && used + fastH > room && fastH <= room) break;
            used += height(p.to, p.from);
        }
        pages.push_back(p);
        i = p.to;
    }
    if (pages.empty()) pages.push_back({});
    return pages;
}

size_t ReaderMenus::pickerPage() {
    // The screen may have changed size since the page was chosen: a page that still exists.
    const size_t pages = pickerPages().size();
    if (picker_ && *picker_ >= pages) picker_ = pages - 1;
    return picker_.value_or(0);
}

Rect ReaderMenus::shownPickerRect() {
    const Rect box = fontPickerRect();
    if (pickerPages().size() > 1) return box;
    const auto rows = fontPickerRows();
    const int bottom = (rows.empty() ? box.y + kPickerTitleH : rows.back().second.y + kPickerRowH) + 30;
    return {box.x, box.y, box.w, bottom - box.y};
}

std::vector<std::pair<std::string, Rect>> ReaderMenus::fontPickerRows() {
    std::vector<std::pair<std::string, Rect>> rows;
    if (!picker_) return rows;
    const auto pages = pickerPages();
    const PickerPage p = pages[pickerPage()];
    const Rect box = fontPickerRect();
    int y = box.y + kPickerTitleH;
    for (size_t i = p.from; i < p.to; ++i) {
        if (headingAbove(i, p.from)) y += kPickerHeadingH;
        rows.push_back({fonts()[i], {box.x, y, box.w, kPickerRowH}});
        y += kPickerRowH;
    }
    return rows;
}

bool ReaderMenus::fontPickerHeadingShown() {
    if (!picker_) return false;
    const PickerPage p = pickerPages()[pickerPage()];
    for (size_t i = p.from; i < p.to; ++i)
        if (headingAbove(i, p.from)) return true;
    return false;
}

Rect ReaderMenus::fontPickerPrevRect() {
    if (!picker_ || pickerPage() == 0) return {};
    const Rect box = fontPickerRect();
    return {box.x, box.y + box.h - kPickerFooterH, box.w / 3, kPickerFooterH};
}

Rect ReaderMenus::fontPickerNextRect() {
    if (!picker_ || pickerPage() + 1 >= pickerPages().size()) return {};
    const Rect box = fontPickerRect();
    return {box.x + box.w - box.w / 3, box.y + box.h - kPickerFooterH, box.w / 3, kPickerFooterH};
}

void ReaderMenus::onPickerTap(Point p, Nav& nav) {
    const Rect box = fontPickerRect();
    pickerPage();
    if (fontPickerPrevRect().contains(p) || fontPickerNextRect().contains(p)) {
        picker_ = *picker_ + (fontPickerNextRect().contains(p) ? 1 : -1);
        nav.damage(box, Waveform::Ui);
        return;
    }
    std::string chosen;
    for (const auto& [font, r] : fontPickerRows())
        if (r.contains(p)) chosen = font;
    if (chosen.empty() && shownPickerRect().contains(p)) return;  // its title, padding or page number: a near miss
    picker_.reset();
    nav.damage(box, Waveform::Quality);  // the page under it, without ghosts
    nav.damage(displayPanelRect(), Waveform::Ui);
    if (chosen.empty() || chosen == reader_.layout().font) return;
    Layout l = reader_.layout();
    l.font = chosen;
    reader_.applyLayout(l, nav);
}

void ReaderMenus::drawPicker(cairo_t* cr) {
    const Rect box = fontPickerRect();
    const auto rows = fontPickerRows();
    const auto pages = pickerPages();
    const bool paged = pages.size() > 1;
    const Rect shown = shownPickerRect();  // as tall as this page's rows, or the whole box when paged
    gray(cr, 255);
    roundedRect(cr, shown.x + 1.5, shown.y + 1.5, shown.w - 3, shown.h - 3, 18);
    cairo_fill_preserve(cr);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    drawText(cr, "Font", {kSans, 9, 700}, {box.x + 50, box.y + 36, box.w - 100, 60}, 1);
    const std::string current = reader_.layout().font;
    const size_t from = pickerPages()[pickerPage()].from;
    for (const auto& [font, r] : rows) {
        const auto it = std::find(fonts().begin(), fonts().end(), font);
        const size_t i = static_cast<size_t>(it - fonts().begin());
        if (headingAbove(i, from))
            drawText(cr, "FAST READING", {kSans, 6.5, 700, false, kSecondary}, {r.x + 50, r.y - kPickerHeadingH + 20, r.w - 100, 44}, 1);
        hRule(cr, r.y, r.x + 30, r.x + r.w - 30, 220);
        centredText(cr, font, {font.c_str(), 9, 400}, {r.x + 50, r.y, r.w - 180, r.h}, Align::Left);
        if (font == current) centredText(cr, "✓", {kSans, 10, 700}, {r.x + r.w - 120, r.y, 70, r.h});
    }
    if (paged) {
        const std::string where = std::to_string(pickerPage() + 1) + " / " + std::to_string(pages.size());
        const int fy = box.y + box.h - kPickerFooterH;
        centredText(cr, where, {kSans, 7.5, 400, false, kSecondary}, {box.x, fy, box.w, kPickerFooterH});
        if (!fontPickerPrevRect().empty()) centredText(cr, "‹", {kSans, 14, 400}, fontPickerPrevRect());
        if (!fontPickerNextRect().empty()) centredText(cr, "›", {kSans, 14, 400}, fontPickerNextRect());
    }
}

bool ReaderMenus::onDisplayTap(Point p, Nav& nav) {
    if (!displayPanelRect().contains(p)) return false;
    Layout l = reader_.layout();
    if (showsTextOptions()) {
        if (fontRowRect().contains(p)) {
            const auto& f = fonts();
            const auto pages = pickerPages();
            picker_ = 0;  // the page with the current font
            for (size_t i = 0; i < pages.size(); ++i)
                for (size_t k = pages[i].from; k < pages[i].to; ++k)
                    if (f[k] == l.font) picker_ = i;
            nav.damage(fontPickerRect(), Waveform::Ui);
            return true;
        }
        if (sizeMinusRect().contains(p)) l.sizePt = std::max(kMinSize, l.sizePt - kSizeStep);
        if (sizePlusRect().contains(p)) l.sizePt = std::min(kMaxSize, l.sizePt + kSizeStep);
        for (int i = 0; i < 3; ++i)
            if (spacingRect(i).contains(p)) l.lineSpacingPct = kSpacings[i];
    }
    if (showsPenOptions())
        for (int i = 0; i < 2; ++i)
            if (penRect(i).contains(p)) {
                reader_.setPenWrites(i == 0, nav);
                nav.damage(displayPanelRect(), Waveform::Ui);
                return true;
            }
    if (reader_.landscape() && showsTextOptions() && orientationRect(2).contains(p)) {
        reader_.setTwoColumns(!reader_.twoColumns(), nav);
        nav.damage(displayPanelRect(), Waveform::Ui);
        return true;
    }
    for (int i = 0; i < 2; ++i)
        if (orientationRect(i).contains(p)) {
            AppSettings s = loadSettings(nav.library());
            s.landscapeReading = landscape_ = i == 1;
            saveSettings(nav.library(), s);
            nav.damage(displayPanelRect(), Waveform::Ui);
            nav.orientationChanged();  // the app turns the screen (a Resize follows)
            return true;
        }
    if (showsTextOptions()) {
        for (int i = 0; i < 4; ++i)
            if (marginRect(i).contains(p)) l.marginLevel = i;
    } else {
        for (int i = 0; i < 3; ++i)
            if (pdfViewRect(i).contains(p)) {
                l.trimMargins = i == 1;
                l.columnView = i == 2;
            }
    }
    reader_.applyLayout(l, nav);
    nav.damage(displayPanelRect(), Waveform::Ui);
    return true;
}

void ReaderMenus::drawDisplay(cairo_t* cr) {
    const Rect d = displayPanelRect();
    fillRect(cr, d, 255);
    hRule(cr, d.y, 0, W(), 190);
    const Layout l = reader_.layout();
    const TextStyle label{kSans, 8, 500};
    if (showsTextOptions()) {
        const Rect fr = fontRowRect();
        centredText(cr, "Font", label, {kMargin, fr.y, 220, 90}, Align::Left);
        gray(cr, 170);
        roundedRect(cr, fr.x + 240 + 1.5, fr.y + 1.5, fr.w - 240 - 3, fr.h - 3, 14);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        centredText(cr, l.font, {l.font.c_str(), 8.5, 400}, {fr.x + 270, fr.y, fr.w - 240 - 120, fr.h}, Align::Left);
        centredText(cr, "›", {kSans, 14, 400}, {fr.x + fr.w - 80, fr.y, 60, fr.h});
        centredText(cr, "Size", label, {kMargin, sizeMinusRect().y, 220, 90}, Align::Left);
        drawButton(cr, sizeMinusRect(), "A−", false);
        drawButton(cr, sizePlusRect(), "A+", false);
        char size[16];
        std::snprintf(size, sizeof size, "%.1f pt", l.sizePt);
        centredText(cr, size, {kSans, 8.5, 400}, {sizeMinusRect().x + 160, sizeMinusRect().y, sizePlusRect().x - sizeMinusRect().x - 160, 90});
        centredText(cr, "Spacing", label, {kMargin, spacingRect(0).y, 220, 90}, Align::Left);
        for (int i = 0; i < 3; ++i)
            drawButton(cr, spacingRect(i), std::to_string(kSpacings[i]) + "%", l.lineSpacingPct == kSpacings[i]);
    }
    if (showsTextOptions()) {
        centredText(cr, "Margins", label, {kMargin, marginRect(0).y, 220, 90}, Align::Left);
        for (int i = 0; i < 4; ++i) drawButton(cr, marginRect(i), kMarginNames[i], l.marginLevel == i);
    } else {
        centredText(cr, "View", label, {kMargin, pdfViewRect(0).y, 220, 90}, Align::Left);
        const int view = l.columnView ? 2 : l.trimMargins ? 1 : 0;
        const char* names[] = {"Page", "Trim", "Columns"};
        for (int i = 0; i < 3; ++i) drawButton(cr, pdfViewRect(i), names[i], view == i);
    }
    if (showsPenOptions()) {  // PDF: in the row EPUB uses for spacing
        centredText(cr, "Pen", label, {kMargin, penRect(0).y, 220, 90}, Align::Left);
        for (int i = 0; i < 2; ++i) drawButton(cr, penRect(i), i ? "Tap" : "Write", reader_.penWrites() == (i == 0));
    }
    centredText(cr, "Orientation", label, {kMargin, orientationRect(0).y, 220, 90}, Align::Left);
    for (int i = 0; i < 2; ++i) drawButton(cr, orientationRect(i), i ? "Landscape" : "Portrait", landscape_ == (i == 1));
    if (reader_.landscape() && showsTextOptions()) drawButton(cr, orientationRect(2), "Two columns", reader_.twoColumns());
}

void ReaderMenus::open(Nav& nav) {
    landscape_ = loadSettings(nav.library()).landscapeReading;
    notes_ = nav.library().annotationsFor(reader_.row().attachmentKey).size();
    nav.damage(topBarRect(), Waveform::Ui);
    nav.damage(panelRect(), Waveform::Ui);
}

void ReaderMenus::close(Nav& nav) {
    nav.damage(topBarRect(), Waveform::Quality);
    nav.damage(panelRect(), Waveform::Quality);
    if (lightOpen_) nav.damage(lightPanelRect(), Waveform::Quality);
    if (displayOpen_) nav.damage(displayPanelRect(), Waveform::Quality);
}

void ReaderMenus::onPage(Nav& nav) {
    nav.damage(ReaderScreen::pageArea(), Waveform::Quality);  // the page under the menus changed
}

void ReaderMenus::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::AnnotationsReady || e.attachmentKey != reader_.row().attachmentKey) return;
    syncing_ = false;
    notes_ = nav.library().annotationsFor(reader_.row().attachmentKey).size();
    nav.damage(panelRect(), Waveform::Ui);
}

int ReaderMenus::sliderPage(int x) const {
    const Rect s = sliderRect();
    const double f = std::clamp(static_cast<double>(x - s.x) / s.w, 0.0, 1.0);
    return static_cast<int>(std::lround(f * std::max(0, reader_.pageCount() - 1)));
}

void ReaderMenus::setLight(int x, Nav& nav) {
    const Rect s = lightSliderRect();
    light_ = static_cast<int>(std::lround(std::clamp(static_cast<double>(x - s.x) / s.w, 0.0, 1.0) * kMaxLight));
    if (lipc_) lipc_->set("com.lab126.powerd", "flIntensity", std::to_string(light_));
    nav.damage(lightPanelRect(), Waveform::Ui);
}

void ReaderMenus::openContents(Nav& nav) {
    ReaderScreen* r = &reader_;
    auto contents = std::make_unique<ContentsScreen>(reader_.toc(), reader_.page(), [r](const TocEntry& t) { r->jumpTo(t.pos); });
    contents->setBookmarks(reader_.bookmarks());
    contents->setPageNumbers([r](int page) { return r->shownPage(page); });
    contents->setGoTo([r](Nav& n) {
        Nav* outer = &n;
        n.push(std::make_unique<GoToPageScreen>(r->shownPageCount(), r->shownPage(r->page()) - 1, [r, outer](int page) {
            outer->pop();  // Contents too: straight back to the book, at the page
            r->goToPage(page);
        }));
    });
    nav.push(std::move(contents));
}

void ReaderMenus::openNotes(Nav& nav) {
    ReaderScreen* r = &reader_;
    nav.push(std::make_unique<BookNotesScreen>(reader_.row(), [r](const AnnotationRecord& a) { r->jumpToAnnotation(a); }));
}

void ReaderMenus::onGesture(const Gesture& g, Nav& nav) {
    const Rect s = sliderRect();
    if (g.type == GestureType::DragStart || g.type == GestureType::DragMove) {
        if (!s.contains(g.start) && !dragPage_) return;
        dragPage_ = sliderPage(g.pos.x);
        nav.damage(panelRect(), Waveform::Fast);
        return;
    }
    if (g.type == GestureType::DragEnd) {
        if (dragPage_) reader_.turnTo(sliderPage(g.pos.x) / reader_.viewsPerPage() * reader_.viewsPerPage());
        dragPage_.reset();
        nav.damage(panelRect(), Waveform::Ui);  // the grays back after the fast drag
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (picker_) return onPickerTap(g.pos, nav);  // over everything: a tap outside only closes it
    if (backRect().contains(g.pos)) {
        reader_.flushInk(nav);  // handwriting becomes annotations first, so the sync sends them
        nav.sync().syncNow();  // closing the book: send what was changed in it, and get Zotero's news
        nav.pop();
        return;
    }
    if (searchRect().contains(g.pos)) {
        nav.push(std::make_unique<BookSearchScreen>(reader_));
        return;
    }
    if (contentsIconRect().contains(g.pos) || buttonRect(0).contains(g.pos)) return openContents(nav);
    if (buttonRect(1).contains(g.pos)) return openNotes(nav);
    if (buttonRect(2).contains(g.pos)) {
        displayOpen_ = !displayOpen_;
        lightOpen_ = false;
        nav.damage(displayPanelRect(), displayOpen_ ? Waveform::Ui : Waveform::Quality);
        return;
    }
    if (displayOpen_ && onDisplayTap(g.pos, nav)) return;
    if (buttonRect(3).contains(g.pos)) {
        lightOpen_ = !lightOpen_;
        if (displayOpen_) nav.damage(displayPanelRect(), Waveform::Quality);
        displayOpen_ = false;
        if (lightOpen_ && lipc_) {
            const std::string v = lipc_->get("com.lab126.powerd", "flIntensity");
            light_ = v.empty() ? -1 : std::atoi(v.c_str());
        }
        nav.damage(lightPanelRect(), lightOpen_ ? Waveform::Ui : Waveform::Quality);
        return;
    }
    if (buttonRect(4).contains(g.pos)) {
        syncing_ = true;
        nav.sync().openBook(reader_.row().key, reader_.row().attachmentKey);
        nav.damage(panelRect(), Waveform::Ui);
        return;
    }
    if (lightOpen_ && lightPanelRect().contains(g.pos)) {
        setLight(g.pos.x, nav);
        return;
    }
    if (s.contains(g.pos)) {
        reader_.turnTo(sliderPage(g.pos.x) / reader_.viewsPerPage() * reader_.viewsPerPage());  // a page's start
        return;
    }
    if (!topBarRect().contains(g.pos) && !panelRect().contains(g.pos) && !(displayOpen_ && displayPanelRect().contains(g.pos)))
        reader_.closeMenus(nav);
}

void ReaderMenus::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    // Top bar.
    const Rect top = topBarRect();
    fillRect(cr, top, 255);
    hRule(cr, top.y + top.h - 2, 0, W(), 190);
    gray(cr, kInk);
    cairo_set_line_width(cr, 6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, kMargin + 22, 52);
    cairo_line_to(cr, kMargin, 74);
    cairo_line_to(cr, kMargin + 22, 96);
    cairo_stroke(cr);
    centredText(cr, "Library", {kSans, 8.5, 500}, {kMargin + 44, 30, 220, 90}, Align::Left);
    centredText(cr, reader_.row().title.empty() ? "Untitled" : reader_.row().title, {kSerif, 9, 700},
                {320, 30, W() - 320 - 260, 90});
    const Rect sr = searchRect();
    searchIcon(cr, sr.x + sr.w / 2.0 - 4, sr.y + sr.h / 2.0 - 4, kInk);
    const Rect ci = contentsIconRect();
    for (int i = 0; i < 3; ++i) fillRect(cr, {ci.x + 18, ci.y + 22 + i * 16, ci.w - 36, 5}, kInk);

    // Light panel (above the bottom panel).
    if (lightOpen_) {
        const Rect lp = lightPanelRect();
        fillRect(cr, lp, 255);
        hRule(cr, lp.y, 0, W(), 190);
        centredText(cr, "Light", {kSans, 8, 500}, {kMargin, lp.y + 40, 110, 90}, Align::Left);
        const Rect ls = lightSliderRect();
        const double f = light_ < 0 ? 0 : static_cast<double>(light_) / kMaxLight;
        progressBar(cr, {ls.x, ls.y + ls.h / 2 - 6, ls.w, 12}, f);
        centredText(cr, light_ < 0 ? "–" : std::to_string(light_), {kSans, 8, 400, false, kSecondary},
                    {ls.x + ls.w + 20, lp.y + 40, 100, 90}, Align::Left);
    }

    if (displayOpen_) drawDisplay(cr);

    // Bottom panel.
    const Rect p = panelRect();
    fillRect(cr, p, 255);
    hRule(cr, p.y, 0, W(), 190);
    const int shown = dragPage_ ? *dragPage_ : reader_.page();
    const std::string where = reader_.type() == DocType::Pdf
                                  ? "Page " + std::to_string(reader_.shownPage(shown)) + " of " + std::to_string(reader_.shownPageCount())
                                  : "Page " + std::to_string(shown + 1) + " of " + std::to_string(reader_.pageCount());
    drawText(cr, reader_.chapter(), {kSans, 8, 500}, {kMargin, p.y + 28, W() - 2 * kMargin - 380, 50}, 1);
    drawText(cr, where, {kSans, 7.5, 400, false, kSecondary}, {W() - kMargin - 380, p.y + 30, 380, 50}, 1, Align::Right);
    const Rect s = sliderRect();
    const double f = reader_.pageCount() > 1 ? static_cast<double>(shown) / (reader_.pageCount() - 1) : 0;
    progressBar(cr, {s.x, s.y + s.h / 2 - 6, s.w, 12}, f);
    gray(cr, kInk);
    cairo_arc(cr, s.x + f * s.w, s.y + s.h / 2.0, 18, 0, 2 * M_PI);
    cairo_fill(cr);
    for (int i = 0; i < 5; ++i) {
        std::string label = kButtons[i];
        if (i == 1) label = notesLabel();
        if (i == 4 && syncing_) label = "Syncing…";
        drawButton(cr, buttonRect(i), label, (i == 3 && lightOpen_) || (i == 2 && displayOpen_));
    }
    if (picker_) drawPicker(cr);
}

}  // namespace zk
