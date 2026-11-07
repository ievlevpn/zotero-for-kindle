#include "screens/reader_menus.h"

#include <algorithm>
#include <cmath>

#include "app/settings_store.h"
#include "docs/fonts.h"
#include "platform/lipc.h"
#include "screens/book_notes.h"
#include "screens/book_search.h"
#include "screens/contents.h"
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
Rect ReaderMenus::fontRect(int i) {
    const int w = (W() - 2 * kMargin - 110) / 4;
    return {kMargin + i * w, H() - kPanelH - kDisplayH + 24, w - 12, 90};
}
Rect ReaderMenus::fontMoreRect() { return {W() - kMargin - 100, H() - kPanelH - kDisplayH + 24, 100, 90}; }
Rect ReaderMenus::sizeMinusRect() { return {kMargin + 240, H() - kPanelH - kDisplayH + 134, 160, 90}; }
Rect ReaderMenus::sizePlusRect() { return {W() - kMargin - 160, H() - kPanelH - kDisplayH + 134, 160, 90}; }
Rect ReaderMenus::marginRect(int i) {
    const int w = (W() - 2 * kMargin - 240) / 4;
    return {kMargin + 240 + i * w, H() - kPanelH - kDisplayH + 244, w - 12, 90};
}
Rect ReaderMenus::trimRect(int i) {
    const int w = (W() - 2 * kMargin - 240) / 2;
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

const std::vector<std::string>& ReaderMenus::fonts() {
    // Scanning fonts reads every file with FreeType: once per process, not per menu.
    static const std::vector<std::string> all = readingFonts(readingFontDirs());
    return all;
}

bool ReaderMenus::onDisplayTap(Point p, Nav& nav) {
    if (!displayPanelRect().contains(p)) return false;
    Layout l = reader_.layout();
    if (showsTextOptions()) {
        for (int i = 0; i < 4; ++i)
            if (fontRect(i).contains(p) && fontOffset_ + i < fonts().size()) l.font = fonts()[fontOffset_ + i];
        if (fontMoreRect().contains(p) && fonts().size() > 4) {
            fontOffset_ = fontOffset_ + 4 < fonts().size() ? fontOffset_ + 4 : 0;
            nav.damage(displayPanelRect(), Waveform::Ui);
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
        for (int i = 0; i < 2; ++i)
            if (trimRect(i).contains(p)) l.trimMargins = i == 1;
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
        for (int i = 0; i < 4 && fontOffset_ + i < fonts().size(); ++i) {
            const std::string& f = fonts()[fontOffset_ + i];
            const Rect r = fontRect(i);
            const bool on = f == l.font;
            gray(cr, on ? kInk : 170);
            roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 14);
            cairo_set_line_width(cr, on ? 5 : 3);
            cairo_stroke(cr);
            centredText(cr, f, {f.c_str(), 8, 400}, {r.x + 8, r.y, r.w - 16, r.h});
        }
        if (fonts().size() > 4) centredText(cr, "›", {kSans, 14, 400}, fontMoreRect());
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
        centredText(cr, "Page margins", label, {kMargin, trimRect(0).y, 220, 90}, Align::Left);
        for (int i = 0; i < 2; ++i) drawButton(cr, trimRect(i), i ? "Trim" : "Keep", l.trimMargins == (i == 1));
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
    nav.push(std::make_unique<ContentsScreen>(reader_.toc(), reader_.page(), [r](const TocEntry& t) { r->jumpTo(t.pos); }));
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
        if (dragPage_) reader_.turnTo(sliderPage(g.pos.x));
        dragPage_.reset();
        nav.damage(panelRect(), Waveform::Ui);  // the grays back after the fast drag
        return;
    }
    if (g.type != GestureType::Tap) return;
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
        reader_.turnTo(sliderPage(g.pos.x));
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
                                  ? "Page " + std::to_string(shown + 1) + " of " + std::to_string(reader_.pageCount())
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
}

}  // namespace zk
