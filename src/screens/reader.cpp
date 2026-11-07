#include "screens/reader.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "app/settings_store.h"
#include "screens/annotation_bar.h"
#include "screens/reader_menus.h"
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
constexpr int kFooterH = 60;
constexpr int kWordsPerMinute = 250;

std::string layoutKey(const std::string& itemKey) { return "layout:" + itemKey; }

// What follows from the window and the book: the page is the window above the footer; an EPUB
// in landscape gets two columns; margins are a share of a column's width.
void fitLayout(Layout& l, DocType type, int twoColumns) {
    l.width = ReaderScreen::pageArea().w;
    l.height = ReaderScreen::pageArea().h;
    // Two columns: EPUBs in landscape, by default only on big screens (Pavel), or as chosen.
    const bool wantTwo = twoColumns < 0 ? l.width >= 2000 : twoColumns == 1;
    l.columns = type == DocType::Epub && l.width > l.height && wantTwo ? 2 : 1;
    l.marginPx = marginPxFor(l.marginLevel, l.width / l.columns);
}

Layout loadLayout(Library& lib, const std::string& itemKey, DocType type, int twoColumns) {
    Layout l;
    auto j = nlohmann::json::parse(lib.metaValue(layoutKey(itemKey)), nullptr, false);
    if (j.is_object()) {
        l.font = j.value("font", l.font);
        l.sizePt = j.value("sizePt", l.sizePt);
        l.lineSpacingPct = j.value("lineSpacingPct", l.lineSpacingPct);
        if (j.contains("marginLevel") && j["marginLevel"].is_number_integer()) l.marginLevel = j["marginLevel"].get<int>();
        else if (j.contains("marginPx") && j["marginPx"].is_number_integer())  // saved before margins were sizes
            l.marginLevel = marginLevelFor(j["marginPx"].get<int>(), 1236);
        l.trimMargins = j.value("trimMargins", false);
    }
    fitLayout(l, type, twoColumns);
    return l;
}

void saveLayout(Library& lib, const std::string& itemKey, const Layout& l) {
    lib.setMetaValue(layoutKey(itemKey), nlohmann::json{{"font", l.font},
                                                        {"sizePt", l.sizePt},
                                                        {"marginLevel", l.marginLevel},
                                                        {"lineSpacingPct", l.lineSpacingPct},
                                                        {"trimMargins", l.trimMargins}}
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
        if (r.empty() || b.kind != HighlightBox::Kind::Highlight) continue;  // drawn below, with cairo
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

    // Text boxes, sticky notes, image areas and ink (PDF).
    cairo_t* cr = c.cr();
    cairo_save(cr);
    const Rect page = ReaderScreen::pageArea();
    cairo_rectangle(cr, page.x, page.y, page.w, page.h);
    cairo_clip(cr);
    for (const auto& b : boxes) {
        switch (b.kind) {
            case HighlightBox::Kind::Highlight:
                break;
            case HighlightBox::Kind::Text:  // its words at their size (pt at 300 dpi from px), in ink
                drawText(cr, b.text, {kSans, std::max(4.0, b.fontPx * 72.0 / 300.0), 400, false, kInk},
                         {b.rect.x, b.rect.y, std::max(b.rect.w, 40), std::max(b.rect.h, b.fontPx + 8)}, 20);
                break;
            case HighlightBox::Kind::Note: {  // a small note, the size of Zotero's icon
                const int s = std::clamp(std::min(b.rect.w, b.rect.h), 36, 60);
                roundedRect(cr, b.rect.x + 1.5, b.rect.y + 1.5, s - 3, s - 3, 6);
                gray(cr, b.shade);
                cairo_fill_preserve(cr);
                gray(cr, kInk);
                cairo_set_line_width(cr, 3);
                cairo_stroke(cr);
                for (int i = 1; i <= 3; ++i) {
                    cairo_move_to(cr, b.rect.x + s * 0.22, b.rect.y + s * (0.22 + i * 0.16));
                    cairo_line_to(cr, b.rect.x + s * 0.78, b.rect.y + s * (0.22 + i * 0.16));
                }
                cairo_set_line_width(cr, 2.5);
                cairo_stroke(cr);
                break;
            }
            case HighlightBox::Kind::Image:  // the area, outlined
                gray(cr, kInk);
                cairo_set_line_width(cr, 4);
                cairo_rectangle(cr, b.rect.x + 2, b.rect.y + 2, b.rect.w - 4, b.rect.h - 4);
                cairo_stroke(cr);
                break;
            case HighlightBox::Kind::Ink:
                gray(cr, kInk);
                cairo_set_line_width(cr, b.strokePx);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                for (const auto& path : b.paths) {
                    for (size_t i = 0; i < path.size(); ++i)
                        i ? cairo_line_to(cr, path[i].x, path[i].y) : cairo_move_to(cr, path[i].x, path[i].y);
                    if (path.size() == 1) cairo_line_to(cr, path[0].x + 0.1, path[0].y);
                }
                cairo_stroke(cr);
                break;
        }
    }
    cairo_restore(cr);
}

void strokePaths(cairo_t* cr, const std::vector<std::vector<Point>>& paths, int widthPx) {
    if (paths.empty()) return;
    gray(cr, kInk);
    cairo_set_line_width(cr, widthPx);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (const auto& path : paths) {
        for (size_t i = 0; i < path.size(); ++i)
            i ? cairo_line_to(cr, path[i].x, path[i].y) : cairo_move_to(cr, path[i].x, path[i].y);
        if (path.size() == 1) cairo_line_to(cr, path[0].x + 0.1, path[0].y);
    }
    cairo_stroke(cr);
}

// Distance from p to the segment a-b, squared.
double distSq(Point p, Point a, Point b) {
    const double vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
    const double len = vx * vx + vy * vy;
    const double t = len > 0 ? std::clamp((wx * vx + wy * vy) / len, 0.0, 1.0) : 0.0;
    const double dx = wx - t * vx, dy = wy - t * vy;
    return dx * dx + dy * dy;
}

bool pathNear(const std::vector<Point>& path, const std::vector<Point>& probe, double radius) {
    const double r2 = radius * radius;
    for (const Point& q : probe)
        for (size_t i = 0; i < path.size(); ++i)
            if (distSq(q, path[i], path[i + 1 < path.size() ? i + 1 : i]) <= r2) return true;
    return false;
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

Rect ReaderScreen::pageArea() { return {0, 0, W(), H() - kFooterH}; }
Rect ReaderScreen::footerArea() { return {0, H() - kFooterH, W(), kFooterH}; }

bool ReaderScreen::wantsKeyboard() const { return note_.has_value() || (editor_ && editor_->wantsKeyboard()); }

void ReaderScreen::onShow(Nav& nav) {
    if (worker_) {  // back from a pushed panel: the book stays open (its notes may have changed)
        refreshHighlights(nav);
        return;
    }
    twoColumnsSetting_ = loadSettings(nav.library()).twoColumns;
    layout_ = loadLayout(nav.library(), row_.key, type_, twoColumnsSetting_);
    if (const std::string c = nav.library().metaValue(kColourKey); !c.empty()) colour_ = c;
    DocPosition start;
    if (auto st = nav.library().readingState(row_.key)) start.value = st->location;
    // Zotero's reading position: where to open a book never read here, or a question if the
    // computer moved on since this Kindle last agreed with it.
    // Held (nothing sent) until it has been compared with the page the book opens on.
    std::string zoteroStart;
    if (auto rp = nav.library().readingPosition(row_.attachmentKey);
        rp && !rp->remote.empty() && (start.value.empty() || rp->local.empty() || rp->remoteNew)) {
        nav.library().holdRemotePosition(row_.attachmentKey);
        if (start.value.empty()) zoteroStart = rp->remote;
        placeAfterFirstPage_ = remoteValue_ = rp->remote;
        remoteVersion_ = rp->remoteVersion;
    }
    worker_ = makeWorker_();
    worker_->open(path_, type_, layout_, start, zoteroStart);
    if (openAt_) {
        turning_ = true;
        worker_->showAnnotation(*openAt_);
        openAt_.reset();
    }
    sendAnnotations(nav, false);
}

void ReaderScreen::onTick(Nav& nav) {
    if (nav.systemInk()) nav.setSystemInk(type_ == DocType::Pdf && penAllowed() && !eraserNear_);
    if (!inkPending_.empty() && !inkDown_ && nav.nowMs() - inkLastMs_ >= kInkCommitMs) commitInk();
    pumpEvents(nav, true);
}

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
            case RenderEvent::Kind::Placed: {
                if (e.requestId != placeId_) break;
                placeId_ = 0;
                if (e.page < 0) {  // can't be placed here: nothing is sent until the reader reads on
                    unplaced_ = true;
                    break;
                }
                remotePage_ = e.page;
                if (e.page == page_) {  // the page we're on: agreed, nothing to ask
                    nav.library().answerRemotePosition(row_.attachmentKey, true, remoteVersion_);
                    break;
                }
                const int pct = static_cast<int>(std::lround(e.progress * 100));
                std::string text = type_ == DocType::Pdf ? "Your computer is on page " + std::to_string(e.page + 1) + "."
                                                         : "Your computer is at " + std::to_string(pct) + "%" +
                                                               (e.chapter.empty() ? "" : " (" + e.chapter + ")") + ".";
                question_ = PositionQuestion{e.page, text + "\nGo there?"};
                if (visible) nav.damage(positionBannerRect(), Waveform::Ui);
                break;
            }
            case RenderEvent::Kind::Ink:
                onInk(e, nav);
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
                inkSaved_.clear();  // now drawn from the annotations
                page_ = e.page;
                pageCount_ = e.pageCount;
                chapter_ = e.chapter;
                progress_ = e.progress;
                wordsLeft_ = e.wordsLeftInChapter;
                position_ = e.position;
                turning_ = false;
                layoutPending_ = false;
                nav.library().saveReading(row_.key, position_.value, progress_);
                // On Zotero's page, Zotero's exact place (not this page's start) stays the Kindle's.
                if (!e.zoteroPosition.empty())
                    nav.library().setLocalPosition(row_.attachmentKey,
                                                   page_ == remotePage_ ? remoteValue_ : e.zoteroPosition);
                if (!placeAfterFirstPage_.empty()) {
                    placeId_ = worker_->placeZotero(std::move(placeAfterFirstPage_));
                    placeAfterFirstPage_.clear();
                }
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

void ReaderScreen::turn(int delta, Nav& nav) {
    if (!worker_ || page_ < 0 || layoutPending_) return;  // old numbering until the new pages arrive
    commitInk();
    const int target = page_ + delta;
    if (target < 0 || (pageCount_ > 0 && target >= pageCount_)) return;
    // Reading on answers Zotero's position: the Kindle's place it is.
    // (Also while Zotero's position is still being placed, or can't be.)
    if (question_) {
        answerPosition(false, nav);
    } else if (unplaced_ || placeId_ || !placeAfterFirstPage_.empty()) {
        unplaced_ = false;
        placeId_ = 0;
        placeAfterFirstPage_.clear();
        nav.library().answerRemotePosition(row_.attachmentKey, false, remoteVersion_);
    }
    turning_ = true;
    worker_->show(target);
}

void ReaderScreen::turnTo(int page) {
    if (!worker_ || layoutPending_) return;
    commitInk();
    turning_ = true;
    worker_->show(page);
}

void ReaderScreen::jumpTo(const DocPosition& pos) {
    menus_.reset();  // we're not visible (a pushed screen called us); the next Page repaints all
    if (!worker_) return;
    commitInk();
    turning_ = true;
    worker_->showPosition(pos);
}

void ReaderScreen::jumpToAnnotation(const AnnotationRecord& a) {
    menus_.reset();
    if (!worker_) {
        openAt_ = a;  // opening from Notes: the worker starts in onShow
        return;
    }
    commitInk();
    turning_ = true;
    worker_->showAnnotation(a);
}

void ReaderScreen::onResize(Nav& nav) {
    commitInk();  // its points belong to the old layout
    fitLayout(layout_, type_, twoColumnsSetting_);
    bar_.reset();  // anchored to the old page
    if (editor_) closeEditor(nav);
    if (!worker_) return;
    layoutPending_ = true;
    worker_->setLayout(layout_, position_);  // keeps the passage; works before the first page too
}

void ReaderScreen::setTwoColumns(bool on, Nav& nav) {
    AppSettings s = loadSettings(nav.library());
    s.twoColumns = twoColumnsSetting_ = on ? 1 : 0;
    saveSettings(nav.library(), s);
    applyLayout(layout_, nav);
}

void ReaderScreen::applyLayout(const Layout& wanted, Nav& nav) {
    commitInk();
    Layout l = wanted;
    fitLayout(l, type_, twoColumnsSetting_);  // the margin size in pixels, the columns
    if (l == layout_ || !worker_ || !ready()) return;
    layoutPending_ = true;
    layout_ = l;
    saveLayout(nav.library(), row_.key, l);
    worker_->setLayout(l, position_);
}

void ReaderScreen::openMenus(Nav& nav) {
    commitInk();  // leaving the book goes through the menus
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
    if (question_ && g.type == GestureType::Tap) {  // reading on (a page turn) is Stay: see turn()
        if (positionGoRect().contains(g.pos)) return answerPosition(true, nav);
        if (positionStayRect().contains(g.pos)) return answerPosition(false, nav);
        if (positionBannerRect().contains(g.pos)) return;
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
                    nav.damage(bar_->rect(), Waveform::Ui);
                    bar_.reset();
                }
                requestSelection(g.pos);
                return;
            case GestureType::DragMove:
                requestSelection(g.pos);
                return;
            case GestureType::DragEnd:
                selDragging_ = false;
                selReleased_ = true;
                requestSelection(g.pos);
                return;
            case GestureType::LongPressUp:
                selReleased_ = true;
                if (selection_ && !bar_) showBar(nav);
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
    // A highlight anywhere opens its editor (Pavel); otherwise the outer 30% turns, the middle opens the menus.
    if (ready() && editAt(g.pos, nav)) return;
    if (g.pos.x > W() * 7 / 10) turn(+1, nav);
    else if (g.pos.x < W() * 3 / 10) turn(-1, nav);
    else if (ready()) openMenus(nav);  // not before the first page
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
    // On a box first; else the nearest box within a fingertip (thin PDF lines are hard to hit).
    constexpr int kSlack = 30;
    std::string key;
    for (auto it = highlights_.rbegin(); it != highlights_.rend() && key.empty(); ++it)
        if (it->rect.contains(p)) key = it->annotationKey;
    if (key.empty()) {
        int best = kSlack + 1;
        for (const auto& h : highlights_) {
            const Rect& r = h.rect;
            const int dx = std::max({r.x - p.x, 0, p.x - (r.x + r.w - 1)});
            const int dy = std::max({r.y - p.y, 0, p.y - (r.y + r.h - 1)});
            if (std::max(dx, dy) < best) {
                best = std::max(dx, dy);
                key = h.annotationKey;
            }
        }
    }
    if (key.empty()) return false;
    auto a = nav.library().annotation(key);
    if (!a) return false;
    Rect anchor;
    for (const auto& h : highlights_)
        if (h.annotationKey == key) anchor = anchor.united(h.rect);
    editor_ = std::make_unique<AnnotationEditor>(*a, anchor);
    nav.damage(editor_->rect(), Waveform::Ui);
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

void ReaderScreen::refreshHighlights(Nav& nav) { sendAnnotations(nav, false); }

// What the page shows of this book's annotations, as one string: a sync that changed nothing of
// them leaves the page alone (no repaint after every background sync).
void ReaderScreen::sendAnnotations(Nav& nav, bool onlyIfChanged) {
    if (!worker_) return;
    std::vector<AnnotationRecord> anns = pageAnnotations(nav.library(), row_.attachmentKey);
    std::string shown;
    for (const auto& a : anns)
        shown += a.key + '\x1f' + a.type + '\x1f' + a.color + '\x1f' + a.comment + '\x1f' + a.position + '\x1e';
    if (onlyIfChanged && shown == shownAnnotations_) return;
    shownAnnotations_ = std::move(shown);
    worker_->highlightsFor(std::move(anns));
}

void ReaderScreen::startSelection(Point at, Nav&) {
    if (!worker_ || page_ < 0) return;
    selecting_ = true;
    selDragging_ = selReleased_ = false;
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
        else if (!selDragging_ && selReleased_ && !bar_) showBar(nav);  // dragged off the text: keep what was selected
        return;
    }
    if (e.selection->text.empty()) {
        selecting_ = false;
        selection_.reset();
        nav.toast("This page has no selectable text.");
        return;
    }
    // Repaint only the lines that changed: a long selection grows a word at a time.
    const std::vector<Rect> before = selection_ ? selection_->rects : std::vector<Rect>{};
    selection_ = e.selection;
    auto has = [](const std::vector<Rect>& v, const Rect& r) { return std::find(v.begin(), v.end(), r) != v.end(); };
    for (const Rect& r : before)
        if (!has(selection_->rects, r)) nav.damage(r, Waveform::Fast);
    for (const Rect& r : selection_->rects)
        if (!has(before, r)) nav.damage(r, Waveform::Fast);
    if (!selDragging_ && selReleased_ && !bar_) showBar(nav);
}

void ReaderScreen::showBar(Nav& nav) {
    if (!selection_) return;
    bar_ = std::make_unique<AnnotationBar>(AnnotationBar::Mode::Create, boundsOf(selection_->rects), colour_);
    nav.damage(bar_->rect(), Waveform::Ui);
}

void ReaderScreen::onBarTap(Point p, Nav& nav) {
    const AnnotationBar::Hit hit = bar_->hit(p);
    switch (hit.action) {
        case AnnotationBar::Action::Colour:
            colour_ = zoteroColours()[hit.colour].hex;
            bar_->setColour(colour_);
            nav.damage(bar_->rect(), Waveform::Ui);
            break;
        case AnnotationBar::Action::Highlight:
            saveSelection("highlight", "", nav);
            break;
        case AnnotationBar::Action::Underline:
            saveSelection("underline", "", nav);
            break;
        case AnnotationBar::Action::Note:
            nav.damage(bar_->rect(), Waveform::Ui);
            bar_.reset();
            note_.emplace();
            nav.keyboard().show();
            noteBox_ = AnnotationEditor::commentRectAbove(nav.keyboard().bounds());
            nav.damage(noteRect(), Waveform::Ui);
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
    if (e.kind == SyncEvent::Kind::AnnotationsReady && e.attachmentKey == row_.attachmentKey) sendAnnotations(nav, false);
    // A sync also brings what changed elsewhere (an annotation deleted in Zotero on the computer).
    if (e.kind == SyncEvent::Kind::SyncFinished) sendAnnotations(nav, true);
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
    fillRect(cr, {0, 0, W(), H()}, 255);
    if (error_) {
        drawBanner(cr, {0, 500, W(), 150}, *error_);
        drawButton(cr, {kMargin, 700, 340, 90}, "Back", true);
        return;
    }
    if (!image_) {
        drawText(cr, "Opening…", {kSerif, 11, 400, false, kSecondary}, {kMargin, 650, W() - 2 * kMargin, 80}, 1, Align::Center);
        return;
    }
    blit(canvas, *image_, 0);
    drawHighlights(canvas, highlights_);
    {
        cairo_save(cr);
        const Rect pa = pageArea();
        cairo_rectangle(cr, pa.x, pa.y, pa.w, pa.h);
        cairo_clip(cr);
        strokePaths(cr, inkSaved_, kInkWidthPx);
        for (const auto& [id, paths] : inkSent_) strokePaths(cr, paths, kInkWidthPx);
        strokePaths(cr, inkPending_, kInkWidthPx);
        if (inkDown_) strokePaths(cr, {inkStroke_}, kInkWidthPx);
        cairo_restore(cr);
    }
    if (selection_) drawSelection(canvas, selection_->rects);
    const Rect f = footerArea();
    drawText(cr, footerLeft(), {kSans, 6.5, 400, false, kTertiary}, {kMargin, f.y + 12, W() - 2 * kMargin - 260, 40}, 1);
    drawText(cr, footerRight(), {kSans, 6.5, 400, false, kTertiary}, {W() - kMargin - 260, f.y + 12, 260, 40}, 1, Align::Right);
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
    if (question_ && !menus_) {
        const Rect b = positionBannerRect();
        roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, 14);
        gray(cr, 255);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 3);
        gray(cr, kInk);
        cairo_stroke(cr);
        const Rect go = positionGoRect();
        drawText(cr, question_->text, {kSans, 8, 500}, {b.x + 36, b.y + 30, go.x - b.x - 60, b.h - 50}, 2);
        drawButton(cr, go, "Go", true);
        drawButton(cr, positionStayRect(), "Stay", false);
    }
    if (menus_) menus_->draw(canvas);
}

std::optional<std::string> ReaderScreen::positionQuestion() const {
    if (!question_) return std::nullopt;
    return question_->text;
}

Rect ReaderScreen::positionBannerRect() { return {kMargin / 2, 30, W() - kMargin, 190}; }
Rect ReaderScreen::positionStayRect() {
    const Rect b = positionBannerRect();
    return {b.x + b.w - 30 - 180, b.y + (b.h - 96) / 2, 180, 96};
}
Rect ReaderScreen::positionGoRect() {
    const Rect s = positionStayRect();
    return {s.x - 24 - 180, s.y, 180, s.h};
}

void ReaderScreen::answerPosition(bool go, Nav& nav) {
    if (!question_) return;
    nav.library().answerRemotePosition(row_.attachmentKey, go, remoteVersion_);
    if (go && worker_) {
        turning_ = true;
        worker_->show(question_->page);
    }
    question_.reset();
    nav.damage(positionBannerRect(), Waveform::Ui);
}

// --- Handwriting -----------------------------------------------------------------------------

bool ReaderScreen::penAllowed() const {
    return worker_ && image_ && !error_ && !menus_ && !selecting_ && !selection_ && !editor_ && !note_ &&
           !layoutPending_ && !turning_;
}

void ReaderScreen::onPen(const PenEvent& p, Nav& nav) {
    const bool eraserNear = p.eraser && p.type != PenEvent::Type::Leave;
    if (eraserNear != eraserNear_) {
        eraserNear_ = eraserNear;
        if (nav.systemInk()) nav.setSystemInk(type_ == DocType::Pdf && penAllowed() && !eraserNear_);
    }
    const bool lift = p.type == PenEvent::Type::Up || p.type == PenEvent::Type::Leave;
    if (lift && wipeFrom_) {
        nav.damage(footerArea().contains(*wipeFrom_) ? footerArea() : Rect{0, 0, W(), H()}, Waveform::Ui);
        wipeFrom_.reset();
    }
    if (lift) {
        if (inkDown_) {
            inkDown_ = false;
            if (!inkStroke_.empty()) inkPending_.push_back(std::move(inkStroke_));
            inkStroke_.clear();
            inkLastMs_ = nav.nowMs();
        }
        if (erasing_) {
            erasing_ = false;
            eraseAlong(eraserPath_, nav);
            eraserPath_.clear();
        }
        return;
    }
    if (p.type == PenEvent::Type::Near) return;
    if (p.type == PenEvent::Type::Down) {
        if (type_ != DocType::Pdf) {
            if (!inkToastShown_ && image_) nav.toast("Handwriting works in PDFs.");
            inkToastShown_ = true;
            return;
        }
        if (!penAllowed() || !pageArea().contains(p.pos)) {
            if (nav.systemInk()) wipeFrom_ = p.pos;  // the system may have inked it: repaint on lift
            return;
        }
        if (p.eraser) {
            erasing_ = true;
            eraserPath_ = {p.pos};
            return;
        }
        if (inkPage_ != page_) commitInk();
        inkPage_ = page_;
        inkDown_ = true;
        inkStroke_ = {p.pos};
        const Rect r{p.pos.x - kInkWidthPx, p.pos.y - kInkWidthPx, 2 * kInkWidthPx + 1, 2 * kInkWidthPx + 1};
        if (!nav.systemInk())  // the system's wet ink already shows the line
            nav.paintNow(r.intersected(pageArea()), Waveform::Ink,
                     [&](cairo_t* cr) { strokePaths(cr, {{p.pos}}, kInkWidthPx); });
        return;
    }
    // Move.
    if (erasing_) {
        if (eraserPath_.empty() || std::abs(eraserPath_.back().x - p.pos.x) + std::abs(eraserPath_.back().y - p.pos.y) >= 4)
            eraserPath_.push_back(p.pos);
        return;
    }
    if (!inkDown_ || inkStroke_.empty()) return;
    const Point a = inkStroke_.back(), b = p.pos;
    if (std::abs(a.x - b.x) + std::abs(a.y - b.y) < 2) return;
    inkStroke_.push_back(b);
    if (nav.systemInk()) return;
    const int pad = kInkWidthPx + 1;
    const Rect r{std::min(a.x, b.x) - pad, std::min(a.y, b.y) - pad, std::abs(a.x - b.x) + 2 * pad + 1,
                 std::abs(a.y - b.y) + 2 * pad + 1};
    nav.paintNow(r.intersected(pageArea()), Waveform::Ink,
                 [&](cairo_t* cr) { strokePaths(cr, {{a, b}}, kInkWidthPx); });
}

void ReaderScreen::commitInk() {
    if (inkPending_.empty() || !worker_ || inkPage_ < 0) return;
    const uint64_t id = worker_->ink(inkPage_, inkPending_, kInkWidthPx);
    inkSent_[id] = std::move(inkPending_);
    inkPending_.clear();
}

void ReaderScreen::onInk(const RenderEvent& e, Nav& nav) {
    auto it = inkSent_.find(e.requestId);
    if (it == inkSent_.end()) return;
    std::vector<std::vector<Point>> paths = std::move(it->second);
    inkSent_.erase(it);
    if (!e.ink) {
        nav.toast("This handwriting couldn't be saved.");
        nav.damage(pageArea(), Waveform::Quality);
        return;
    }
    AnnotationRecord a;
    a.parentKey = row_.attachmentKey;
    a.type = "ink";
    a.color = "#000000";
    a.position = e.ink->position;
    a.sortIndex = e.ink->sortIndex;
    a.pageLabel = e.ink->pageLabel;
    nav.library().createAnnotation(a);
    nav.sync().pushSoon();
    if (e.page == page_) inkSaved_.insert(inkSaved_.end(), paths.begin(), paths.end());
    refreshHighlights(nav);
}

// The eraser end: strokes not yet saved go at once; saved ink annotations it touches are deleted.
void ReaderScreen::eraseAlong(const std::vector<Point>& path, Nav& nav) {
    if (path.empty()) return;
    // A quick swipe leaves samples far apart: test the line between them too, every few pixels.
    std::vector<Point> probe{path.front()};
    for (size_t i = 1; i < path.size(); ++i) {
        const Point a = path[i - 1], b = path[i];
        const int steps = std::max(1, (std::abs(b.x - a.x) + std::abs(b.y - a.y)) / 4);
        for (int k = 1; k <= steps; ++k)
            probe.push_back({a.x + (b.x - a.x) * k / steps, a.y + (b.y - a.y) * k / steps});
    }
    const double radius = kInkWidthPx / 2.0 + 12;
    const size_t before = inkPending_.size();
    inkPending_.erase(std::remove_if(inkPending_.begin(), inkPending_.end(),
                                     [&](const std::vector<Point>& path) { return pathNear(path, probe, radius); }),
                      inkPending_.end());
    bool changed = inkPending_.size() != before;
    std::vector<std::string> keys;
    for (const auto& b : highlights_) {
        if (b.kind != HighlightBox::Kind::Ink) continue;
        const double r = b.strokePx / 2.0 + 12;
        for (const auto& path : b.paths)
            if (pathNear(path, probe, r)) {
                keys.push_back(b.annotationKey);
                break;
            }
    }
    for (const auto& k : keys) nav.library().deleteAnnotation(k);
    if (!keys.empty()) {
        nav.sync().pushSoon();
        inkSaved_.clear();
        highlights_.erase(std::remove_if(highlights_.begin(), highlights_.end(),
                                         [&](const HighlightBox& b) {
                                             return std::find(keys.begin(), keys.end(), b.annotationKey) != keys.end();
                                         }),
                          highlights_.end());
        refreshHighlights(nav);
        changed = true;
    }
    if (changed) nav.damage(pageArea(), Waveform::Quality);
}

}  // namespace zk
