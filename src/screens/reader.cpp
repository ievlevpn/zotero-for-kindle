#include "screens/reader.h"

#include <cmath>

#include <nlohmann/json.hpp>

#include "screens/annotation_bar.h"
#include "screens/reader_menus.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236, kH = 1547, kFooterH = 60;
constexpr int kWordsPerMinute = 250;

std::string layoutKey(const std::string& itemKey) { return "layout:" + itemKey; }

Layout loadLayout(Library& lib, const std::string& itemKey) {
    Layout l;
    auto j = nlohmann::json::parse(lib.metaValue(layoutKey(itemKey)), nullptr, false);
    if (!j.is_object()) return l;
    l.font = j.value("font", l.font);
    l.sizePt = j.value("sizePt", l.sizePt);
    l.marginPx = j.value("marginPx", l.marginPx);
    l.lineSpacingPct = j.value("lineSpacingPct", l.lineSpacingPct);
    return l;
}

void saveLayout(Library& lib, const std::string& itemKey, const Layout& l) {
    lib.setMetaValue(layoutKey(itemKey), nlohmann::json{{"font", l.font}, {"sizePt", l.sizePt}, {"marginPx", l.marginPx},
                                                        {"lineSpacingPct", l.lineSpacingPct}}
                                             .dump());
}

void blit(Canvas& c, const PageImage& img, int y0) {
    cairo_surface_flush(c.surface());
    unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (int y = 0; y < img.height && y + y0 < c.height(); ++y) {
        auto* row = reinterpret_cast<uint32_t*>(data + (y + y0) * stride);
        const uint8_t* src = img.gray.data() + static_cast<size_t>(y) * img.width;
        for (int x = 0; x < img.width && x < c.width(); ++x) row[x] = 0xff000000u | (src[x] << 16) | (src[x] << 8) | src[x];
    }
    cairo_surface_mark_dirty(c.surface());
}

// Zotero highlights (read-only): the page under the box is multiplied by the shade, so paper
// darkens and ink stays black; underlines are a bar under the text.
void drawHighlights(Canvas& c, const std::vector<HighlightBox>& boxes) {
    if (boxes.empty()) return;
    cairo_surface_flush(c.surface());
    unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (const auto& b : boxes) {
        const Rect r = b.rect.intersected(ReaderScreen::pageArea());
        if (r.empty()) continue;
        if (b.underline) {
            for (int y = std::max(r.y, r.y + r.h - 5); y < r.y + r.h + 1 && y < c.height(); ++y) {
                auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
                for (int x = r.x; x < r.x + r.w; ++x) row[x] = 0xff3c3c3cu;
            }
            continue;
        }
        for (int y = r.y; y < r.y + r.h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
            for (int x = r.x; x < r.x + r.w; ++x) {
                const uint32_t v = (row[x] & 0xff) * b.shade / 255;
                row[x] = 0xff000000u | (v << 16) | (v << 8) | v;
            }
        }
    }
    cairo_surface_mark_dirty(c.surface());
}

// The selection, shown inverted (white text on black), as Kindle shows it.
void drawSelection(Canvas& c, const std::vector<Rect>& rects) {
    if (rects.empty()) return;
    cairo_surface_flush(c.surface());
    unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (const Rect& box : rects) {
        const Rect r = box.intersected(ReaderScreen::pageArea());
        for (int y = r.y; y < r.y + r.h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
            for (int x = r.x; x < r.x + r.w; ++x) row[x] = 0xff000000u | (~row[x] & 0x00ffffffu);
        }
    }
    cairo_surface_mark_dirty(c.surface());
}

Rect boundsOf(const std::vector<Rect>& rects) {
    Rect out;
    for (const Rect& r : rects) out = out.united(r);
    return out;
}

constexpr const char* kColourKey = "annotationColour";

// What the page shows: not conflict copies (they sit over Zotero's version and are resolved in Notes).
std::vector<AnnotationRecord> pageAnnotations(Library& lib, const std::string& attachmentKey) {
    std::vector<AnnotationRecord> out;
    for (auto& a : lib.annotationsFor(attachmentKey))
        if (!a.conflict) out.push_back(std::move(a));
    return out;
}

}  // namespace

ReaderScreen::ReaderScreen(ItemRow row, std::string path, DocType type, WorkerFactory makeWorker, int flashEvery,
                           Lipc* lipc)
    : row_(std::move(row)), path_(std::move(path)), type_(type), makeWorker_(std::move(makeWorker)),
      flashEvery_(flashEvery > 0 ? flashEvery : 6), lipc_(lipc) {}

ReaderScreen::~ReaderScreen() = default;

Rect ReaderScreen::pageArea() { return {0, 0, kW, kH - kFooterH}; }
Rect ReaderScreen::footerArea() { return {0, kH - kFooterH, kW, kFooterH}; }
Rect ReaderScreen::noteRect() { return AnnotationEditor::commentRect(); }  // above the system keyboard

bool ReaderScreen::wantsKeyboard() const { return note_.has_value() || (editor_ && editor_->wantsKeyboard()); }

void ReaderScreen::onShow(Nav& nav) {
    if (worker_) {  // back from a pushed panel: the book stays open (its notes may have changed)
        refreshHighlights(nav);
        return;
    }
    layout_ = loadLayout(nav.library(), row_.key);
    if (const std::string c = nav.library().metaValue(kColourKey); !c.empty()) colour_ = c;
    DocPosition start;
    if (auto st = nav.library().readingState(row_.key)) start.value = st->location;
    worker_ = makeWorker_();
    worker_->open(path_, type_, layout_, start);
    worker_->highlightsFor(pageAnnotations(nav.library(), row_.attachmentKey));
}

void ReaderScreen::onTick(Nav& nav) { pumpEvents(nav, true); }

void ReaderScreen::pumpEvents(Nav& nav, bool visible) {
    if (!worker_) return;
    RenderEvent e;
    while (worker_->poll(e)) {
        switch (e.kind) {
            case RenderEvent::Kind::Opened:
                pageCount_ = e.pageCount;
                toc_ = e.toc;
                break;
            case RenderEvent::Kind::Error:
                error_ = e.error;
                if (visible) nav.redrawAll(Waveform::Quality);
                break;
            case RenderEvent::Kind::SearchResults:
                if (searchListener_) searchListener_(e.requestId, e.hits);
                break;
            case RenderEvent::Kind::Selection:
                onSelection(e, nav);
                break;
            case RenderEvent::Kind::HitAnnotation:
                break;
            case RenderEvent::Kind::Page: {
                const bool first = image_ == nullptr;
                const bool chapterChanged = !first && e.chapter != chapter_;
                const bool turned = turning_;
                if (e.page != page_ && (selecting_ || selection_ || editor_)) {  // the page changed under them
                    if (note_ || (editor_ && editor_->wantsKeyboard())) nav.keyboard().hide();
                    editor_.reset();
                    selecting_ = selDragging_ = false;
                    selection_.reset();
                    bar_.reset();
                    note_.reset();
                }
                image_ = e.image;
                highlights_ = e.highlights;
                page_ = e.page;
                pageCount_ = e.pageCount;
                chapter_ = e.chapter;
                progress_ = e.progress;
                wordsLeft_ = e.wordsLeftInChapter;
                position_ = e.position;
                turning_ = false;
                layoutPending_ = false;
                nav.library().saveReading(row_.key, position_.value, progress_);
                if (!visible) break;
                Waveform w = Waveform::Quality;
                if (turned && (++turns_ % flashEvery_ == 0 || chapterChanged)) w = Waveform::Full;
                if (!turned && chapterChanged) w = Waveform::Full;
                if (menus_) {
                    menus_->onPage(nav);
                } else {
                    nav.damage(pageArea(), w);
                    nav.damage(footerArea(), Waveform::Quality);
                }
                break;
            }
        }
    }
    if (menus_ && visible) menus_->onTick(nav);
}

void ReaderScreen::turn(int delta, Nav&) {
    if (!worker_ || page_ < 0 || layoutPending_) return;  // old numbering until the new pages arrive
    const int target = page_ + delta;
    if (target < 0 || (pageCount_ > 0 && target >= pageCount_)) return;
    turning_ = true;
    worker_->show(target);
}

void ReaderScreen::turnTo(int page) {
    if (!worker_ || layoutPending_) return;
    turning_ = true;
    worker_->show(page);
}

void ReaderScreen::jumpTo(const DocPosition& pos) {
    menus_.reset();  // we're not visible (a pushed screen called us); the next Page repaints all
    if (!worker_) return;
    turning_ = true;
    worker_->showPosition(pos);
}

void ReaderScreen::jumpToAnnotation(const AnnotationRecord& a) {
    menus_.reset();
    if (!worker_) return;
    turning_ = true;
    worker_->showAnnotation(a);
}

void ReaderScreen::applyLayout(const Layout& l, Nav& nav) {
    if (l == layout_ || !worker_ || !ready()) return;
    layoutPending_ = true;
    layout_ = l;
    saveLayout(nav.library(), row_.key, l);
    worker_->setLayout(l, position_);
}

void ReaderScreen::openMenus(Nav& nav) {
    menus_ = std::make_unique<ReaderMenus>(*this, lipc_);
    menus_->open(nav);
}

void ReaderScreen::closeMenus(Nav& nav) {
    if (!menus_) return;
    menus_->close(nav);
    menus_.reset();
}

void ReaderScreen::onGesture(const Gesture& g, Nav& nav) {
    if (error_) {
        if (g.type == GestureType::Tap) nav.pop();  // "Back"
        return;
    }
    if (menus_) {
        menus_->onGesture(g, nav);
        return;
    }
    if (editor_) {
        if (g.type == GestureType::Tap && editor_->onTap(g.pos, nav) == AnnotationEditor::Result::Open) {
            if (editor_->takeChanged()) {
                refreshHighlights(nav);
                nav.sync().pushSoon();
            }
            return;
        }
        closeEditor(nav);
        if (g.type == GestureType::Tap) return;
    }
    if (note_) {  // typing a note: a tap elsewhere gives up on it
        if (g.type == GestureType::Tap && !noteRect().contains(g.pos)) closeSelection(nav);
        return;
    }
    if (selecting_ || selection_) {
        switch (g.type) {
            case GestureType::DragStart:
                selDragging_ = true;
                if (bar_) {
                    nav.damage(bar_->rect(), Waveform::Fast);
                    bar_.reset();
                }
                requestSelection(g.pos);
                return;
            case GestureType::DragMove:
                requestSelection(g.pos);
                return;
            case GestureType::DragEnd:
                selDragging_ = false;
                requestSelection(g.pos);
                return;
            case GestureType::Tap:
                if (bar_ && bar_->rect().contains(g.pos)) onBarTap(g.pos, nav);
                else closeSelection(nav);
                return;
            case GestureType::LongPress:
                closeSelection(nav);
                break;  // and select anew below
            case GestureType::Swipe:
                closeSelection(nav);
                return;
        }
    }
    if (g.type == GestureType::LongPress) {
        if (ready() && pageArea().contains(g.pos)) startSelection(g.pos, nav);
        return;
    }
    if (g.type == GestureType::Swipe) {
        if (g.dir == Direction::Left) turn(+1, nav);
        if (g.dir == Direction::Right) turn(-1, nav);
        return;
    }
    if (g.type != GestureType::Tap) return;
    // The outer 30% always turns; in the middle a highlight opens its editor, elsewhere the menus.
    if (g.pos.x > kW * 7 / 10) turn(+1, nav);
    else if (g.pos.x < kW * 3 / 10) turn(-1, nav);
    else if (ready() && !editAt(g.pos, nav)) openMenus(nav);  // not before the first page
}

void ReaderScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (editor_) {
        if (editor_->onKey(k, nav) == AnnotationEditor::Result::Closed) closeEditor(nav);
        return;
    }
    if (note_) {
        if (k.code == KeyEvent::Code::Escape) return closeSelection(nav);
        note_->apply(k);
        if (note_->submitted()) return saveSelection("highlight", note_->text(), nav);
        nav.damage(noteRect(), Waveform::Fast);
        return;
    }
    if (menus_) menus_->onKey(k, nav);
}

bool ReaderScreen::editAt(Point p, Nav& nav) {
    std::string key;
    for (auto it = highlights_.rbegin(); it != highlights_.rend() && key.empty(); ++it)
        if (it->rect.contains(p)) key = it->annotationKey;
    if (key.empty()) return false;
    auto a = nav.library().annotation(key);
    if (!a) return false;
    Rect anchor;
    for (const auto& h : highlights_)
        if (h.annotationKey == key) anchor = anchor.united(h.rect);
    editor_ = std::make_unique<AnnotationEditor>(*a, anchor);
    nav.damage(editor_->rect(), Waveform::Fast);
    return true;
}

void ReaderScreen::closeEditor(Nav& nav) {
    if (!editor_) return;
    if (editor_->takeChanged()) {
        refreshHighlights(nav);
        nav.sync().pushSoon();
    }
    editor_.reset();
    nav.damage(pageArea(), Waveform::Quality);
}

void ReaderScreen::refreshHighlights(Nav& nav) {
    if (worker_) worker_->highlightsFor(pageAnnotations(nav.library(), row_.attachmentKey));
}

void ReaderScreen::startSelection(Point at, Nav&) {
    if (!worker_ || page_ < 0) return;
    selecting_ = true;
    selDragging_ = false;
    selAnchor_ = at;
    requestSelection(at);
}

void ReaderScreen::requestSelection(Point to) {
    if (worker_ && selecting_) selPending_ = worker_->select(page_, selAnchor_, to);
}

void ReaderScreen::onSelection(const RenderEvent& e, Nav& nav) {
    if (!selecting_ || e.requestId != selPending_ || e.page != page_) return;
    if (!e.selection) {
        if (!selection_) selecting_ = false;    // the long-press wasn't on text
        else if (!selDragging_ && !bar_) showBar(nav);  // dragged off the text: keep what was selected
        return;
    }
    if (e.selection->text.empty()) {
        selecting_ = false;
        selection_.reset();
        nav.toast("This page has no selectable text.");
        return;
    }
    const Rect before = selection_ ? boundsOf(selection_->rects) : Rect{};
    selection_ = e.selection;
    nav.damage(before.united(boundsOf(selection_->rects)), Waveform::Fast);
    if (!selDragging_) showBar(nav);
}

void ReaderScreen::showBar(Nav& nav) {
    if (!selection_) return;
    bar_ = std::make_unique<AnnotationBar>(AnnotationBar::Mode::Create, boundsOf(selection_->rects), colour_);
    nav.damage(bar_->rect(), Waveform::Fast);
}

void ReaderScreen::onBarTap(Point p, Nav& nav) {
    const AnnotationBar::Hit hit = bar_->hit(p);
    switch (hit.action) {
        case AnnotationBar::Action::Colour:
            colour_ = zoteroColours()[hit.colour].hex;
            bar_->setColour(colour_);
            nav.damage(bar_->rect(), Waveform::Fast);
            break;
        case AnnotationBar::Action::Highlight:
            saveSelection("highlight", "", nav);
            break;
        case AnnotationBar::Action::Underline:
            saveSelection("underline", "", nav);
            break;
        case AnnotationBar::Action::Note:
            nav.damage(bar_->rect(), Waveform::Fast);
            bar_.reset();
            note_.emplace();
            nav.keyboard().show();
            nav.damage(noteRect(), Waveform::Fast);
            break;
        default:
            break;
    }
}

void ReaderScreen::saveSelection(const std::string& type, const std::string& comment, Nav& nav) {
    if (!selection_) return closeSelection(nav);
    AnnotationRecord a;
    a.parentKey = row_.attachmentKey;
    a.type = type;
    a.text = selection_->text;
    a.comment = comment;
    a.color = colour_;
    a.sortIndex = selection_->sortIndex;
    a.position = selection_->position;
    a.pageLabel = selection_->pageLabel;
    a.localOnly = !selection_->roundTrips;  // §7.5: never upload a place that doesn't round-trip
    nav.library().createAnnotation(a);
    nav.library().setMetaValue(kColourKey, colour_);
    if (a.localOnly) nav.toast("Saved on the Kindle only — this place can't be synced exactly.");
    else nav.sync().pushSoon();
    refreshHighlights(nav);
    closeSelection(nav);
}

void ReaderScreen::closeSelection(Nav& nav) {
    if (note_) nav.keyboard().hide();
    selecting_ = selDragging_ = false;
    selection_.reset();
    bar_.reset();
    note_.reset();
    nav.damage(pageArea(), Waveform::Quality);
}

void ReaderScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind == SyncEvent::Kind::AnnotationsReady && e.attachmentKey == row_.attachmentKey && worker_)
        worker_->highlightsFor(pageAnnotations(nav.library(), row_.attachmentKey));
    if (menus_) menus_->onSyncEvent(e, nav);
}

std::string ReaderScreen::footerLeft() const {
    if (chapter_.empty() && wordsLeft_ <= 0) return {};
    const int64_t minutes = static_cast<int64_t>(std::lround(static_cast<double>(wordsLeft_) / kWordsPerMinute));
    const std::string left = minutes >= 1 ? std::to_string(minutes) + " min left in chapter" : "Less than a minute left";
    return chapter_.empty() ? left : chapter_ + " · " + left;
}

std::string ReaderScreen::footerRight() const {
    if (type_ == DocType::Pdf) return "p. " + std::to_string(page_ + 1) + " of " + std::to_string(pageCount_);
    return std::to_string(static_cast<int>(progress_ * 100)) + "%";
}

void ReaderScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, kH}, 255);
    if (error_) {
        drawBanner(cr, {0, 500, kW, 150}, *error_);
        drawButton(cr, {kMargin, 700, 340, 90}, "Back", true);
        return;
    }
    if (!image_) {
        drawText(cr, "Opening…", {kSerif, 11, 400, false, kSecondary}, {kMargin, 650, kW - 2 * kMargin, 80}, 1, Align::Center);
        return;
    }
    blit(canvas, *image_, 0);
    drawHighlights(canvas, highlights_);
    if (selection_) drawSelection(canvas, selection_->rects);
    const Rect f = footerArea();
    drawText(cr, footerLeft(), {kSans, 6.5, 400, false, kTertiary}, {kMargin, f.y + 12, kW - 2 * kMargin - 260, 40}, 1);
    drawText(cr, footerRight(), {kSans, 6.5, 400, false, kTertiary}, {kW - kMargin - 260, f.y + 12, 260, 40}, 1, Align::Right);
    if (bar_) bar_->draw(cr);
    if (editor_) editor_->draw(cr);
    if (note_) {
        const Rect n = noteRect();
        roundedRect(cr, n.x + 1.5, n.y + 1.5, n.w - 3, n.h - 3, 18);
        gray(cr, 255);
        cairo_fill_preserve(cr);
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        drawText(cr, "Note", {kSans, 8, 500}, {n.x + 30, n.y + 24, n.w - 60, 50}, 1);
        const bool empty = note_->text().empty();
        drawText(cr, empty ? "Type a note…" : note_->text(), {kSerif, 9, 400, false, empty ? kTertiary : kInk},
                 {n.x + 30, n.y + 84, n.w - 60, 110}, 2);
        drawText(cr, "Return saves", {kSans, 6, 400, false, kTertiary}, {n.x + 30, n.y + n.h - 56, n.w - 60, 40}, 1,
                 Align::Right);
    }
    if (menus_) menus_->draw(canvas);
}

}  // namespace zk
