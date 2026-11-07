#include "screens/reader_menus.h"

#include <algorithm>
#include <cmath>

#include "platform/lipc.h"
#include "screens/book_notes.h"
#include "screens/contents.h"
#include "screens/reader.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236, kH = 1547;
constexpr int kTopH = 140, kPanelH = 380, kLightH = 170;
const char* kButtons[] = {"Contents", "Notes", "Display", "Light", "Sync"};
constexpr int kMaxLight = 24;  // com.lab126.powerd flIntensity range on the PW5

}  // namespace

Rect ReaderMenus::topBarRect() { return {0, 0, kW, kTopH}; }
Rect ReaderMenus::panelRect() { return {0, kH - kPanelH, kW, kPanelH}; }
Rect ReaderMenus::backRect() { return {0, 0, 320, kTopH}; }
Rect ReaderMenus::searchRect() { return {kW - kMargin - 190, 30, 80, 80}; }
Rect ReaderMenus::contentsIconRect() { return {kW - kMargin - 90, 30, 90, 80}; }
Rect ReaderMenus::sliderRect() { return {kMargin, kH - kPanelH + 100, kW - 2 * kMargin, 70}; }
Rect ReaderMenus::buttonRect(int i) {
    const int w = (kW - 2 * kMargin) / 5;
    return {kMargin + i * w, kH - kPanelH + 210, w - 12, 120};
}
Rect ReaderMenus::lightPanelRect() { return {0, kH - kPanelH - kLightH, kW, kLightH}; }
Rect ReaderMenus::lightSliderRect() { return {kMargin + 120, kH - kPanelH - kLightH + 50, kW - 2 * kMargin - 240, 70}; }

void ReaderMenus::open(Nav& nav) {
    notes_ = nav.library().annotationsFor(reader_.row().attachmentKey).size();
    nav.damage(topBarRect(), Waveform::Fast);
    nav.damage(panelRect(), Waveform::Fast);
}

void ReaderMenus::close(Nav& nav) {
    nav.damage(topBarRect(), Waveform::Quality);
    nav.damage(panelRect(), Waveform::Quality);
    if (lightOpen_) nav.damage(lightPanelRect(), Waveform::Quality);
}

void ReaderMenus::onPage(Nav& nav) {
    nav.damage(ReaderScreen::pageArea(), Waveform::Quality);  // the page under the menus changed
}

void ReaderMenus::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::AnnotationsReady || e.attachmentKey != reader_.row().attachmentKey) return;
    syncing_ = false;
    notes_ = nav.library().annotationsFor(reader_.row().attachmentKey).size();
    nav.damage(panelRect(), Waveform::Fast);
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
    nav.damage(lightPanelRect(), Waveform::Fast);
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
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (backRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (contentsIconRect().contains(g.pos) || buttonRect(0).contains(g.pos)) return openContents(nav);
    if (buttonRect(1).contains(g.pos)) return openNotes(nav);
    if (buttonRect(2).contains(g.pos)) return;  // Display: the display-settings task
    if (buttonRect(3).contains(g.pos)) {
        lightOpen_ = !lightOpen_;
        if (lightOpen_ && lipc_) {
            const std::string v = lipc_->get("com.lab126.powerd", "flIntensity");
            light_ = v.empty() ? -1 : std::atoi(v.c_str());
        }
        nav.damage(lightPanelRect(), lightOpen_ ? Waveform::Fast : Waveform::Quality);
        return;
    }
    if (buttonRect(4).contains(g.pos)) {
        syncing_ = true;
        nav.sync().openBook(reader_.row().key, reader_.row().attachmentKey);
        nav.damage(panelRect(), Waveform::Fast);
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
    if (!topBarRect().contains(g.pos) && !panelRect().contains(g.pos)) reader_.closeMenus(nav);
}

void ReaderMenus::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    // Top bar.
    const Rect top = topBarRect();
    fillRect(cr, top, 255);
    hRule(cr, top.y + top.h - 2, 0, kW, 190);
    gray(cr, kInk);
    cairo_set_line_width(cr, 6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, kMargin + 22, 52);
    cairo_line_to(cr, kMargin, 74);
    cairo_line_to(cr, kMargin + 22, 96);
    cairo_stroke(cr);
    centredText(cr, "Library", {kSans, 8.5, 500}, {kMargin + 44, 30, 220, 90}, Align::Left);
    centredText(cr, reader_.row().title.empty() ? "Untitled" : reader_.row().title, {kSerif, 9, 700},
                {320, 30, kW - 320 - 260, 90});
    const Rect sr = searchRect();
    searchIcon(cr, sr.x + sr.w / 2.0 - 4, sr.y + sr.h / 2.0 - 4, kInk);
    const Rect ci = contentsIconRect();
    for (int i = 0; i < 3; ++i) fillRect(cr, {ci.x + 18, ci.y + 22 + i * 16, ci.w - 36, 5}, kInk);

    // Light panel (above the bottom panel).
    if (lightOpen_) {
        const Rect lp = lightPanelRect();
        fillRect(cr, lp, 255);
        hRule(cr, lp.y, 0, kW, 190);
        centredText(cr, "Light", {kSans, 8, 500}, {kMargin, lp.y + 40, 110, 90}, Align::Left);
        const Rect ls = lightSliderRect();
        const double f = light_ < 0 ? 0 : static_cast<double>(light_) / kMaxLight;
        progressBar(cr, {ls.x, ls.y + ls.h / 2 - 6, ls.w, 12}, f);
        centredText(cr, light_ < 0 ? "–" : std::to_string(light_), {kSans, 8, 400, false, kSecondary},
                    {ls.x + ls.w + 20, lp.y + 40, 100, 90}, Align::Left);
    }

    // Bottom panel.
    const Rect p = panelRect();
    fillRect(cr, p, 255);
    hRule(cr, p.y, 0, kW, 190);
    const int shown = dragPage_ ? *dragPage_ : reader_.page();
    const std::string where = reader_.type() == DocType::Pdf
                                  ? "Page " + std::to_string(shown + 1) + " of " + std::to_string(reader_.pageCount())
                                  : "Page " + std::to_string(shown + 1) + " of " + std::to_string(reader_.pageCount());
    drawText(cr, reader_.chapter(), {kSans, 8, 500}, {kMargin, p.y + 28, kW - 2 * kMargin - 380, 50}, 1);
    drawText(cr, where, {kSans, 7.5, 400, false, kSecondary}, {kW - kMargin - 380, p.y + 30, 380, 50}, 1, Align::Right);
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
        drawButton(cr, buttonRect(i), label, (i == 3 && lightOpen_));
    }
}

}  // namespace zk
