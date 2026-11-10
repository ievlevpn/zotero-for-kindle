#include "screens/reader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "app/settings_store.h"
#include "core/log.h"
#include "screens/annotation_bar.h"
#include "screens/note_popup.h"
#include "plugins/events.h"
#include "plugins/identifiers.h"
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
        l.columnView = j.value("columnView", false);
    }
    fitLayout(l, type, twoColumns);
    return l;
}

void saveLayout(Library& lib, const std::string& itemKey, const Layout& l) {
    lib.setMetaValue(layoutKey(itemKey), nlohmann::json{{"font", l.font},
                                                        {"sizePt", l.sizePt},
                                                        {"marginLevel", l.marginLevel},
                                                        {"lineSpacingPct", l.lineSpacingPct},
                                                        {"trimMargins", l.trimMargins},
                                                        {"columnView", l.columnView}}
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

// An area's frame: its colour's shade made darker, so even yellow shows as a line on e-ink while
// the colours stay apart (yellow 121, red 94, purple 88).
uint8_t areaFrameShade(uint8_t shade) { return static_cast<uint8_t>(shade * 55 / 100); }

// A sticky note's icon as drawn: a square at its box's top-left, the size of Zotero's icon.
Rect noteIcon(const Rect& r) {
    const int s = std::clamp(std::min(r.w, r.h), 36, 60);
    return {r.x, r.y, s, s};
}

// The four edges of a frame drawn `w` px wide around `r`, padded: what a moving frame repaints.
std::vector<Rect> frameEdges(Rect r, int w) {
    if (r.empty()) return {};
    const int p = w + 4;
    return {{r.x - p, r.y - p, r.w + 2 * p, 2 * p},
            {r.x - p, r.y + r.h - p, r.w + 2 * p, 2 * p},
            {r.x - p, r.y - p, 2 * p, r.h + 2 * p},
            {r.x + r.w - p, r.y - p, 2 * p, r.h + 2 * p}};
}

void strokeFrame(cairo_t* cr, Rect r, uint8_t shade, int w) {
    gray(cr, shade);
    cairo_set_line_width(cr, w);
    cairo_rectangle(cr, r.x + w / 2.0, r.y + w / 2.0, r.w - w, r.h - w);
    cairo_stroke(cr);
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
        if (!r.empty() && b.kind == HighlightBox::Kind::Image) {  // an area: a light tint (its frame below)
            const uint32_t tint = 255 - (255 - b.shade) / 3;
            for (int y = r.y; y < r.y + r.h; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
                for (int x = r.x; x < r.x + r.w; ++x) {
                    const uint32_t v = (row[x] & 0xff) * tint / 255;
                    row[x] = 0xff000000u | (v << 16) | (v << 8) | v;
                }
            }
            continue;
        }
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
                const int s = noteIcon(b.rect).w;
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
            case HighlightBox::Kind::Image:  // the area: a 3 px frame, its colour as a darker shade
                gray(cr, areaFrameShade(b.shade));
                cairo_set_line_width(cr, 3);
                cairo_rectangle(cr, b.rect.x + 1.5, b.rect.y + 1.5, b.rect.w - 3, b.rect.h - 3);
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

double distSq(Point p, Point a, Point b);

// Douglas-Peucker: drops points within `eps` px of the line through their neighbours.
void simplify(const std::vector<Point>& in, size_t from, size_t to, double eps, std::vector<bool>& keep) {
    if (to <= from + 1) return;
    double worst = -1;
    size_t at = from;
    for (size_t i = from + 1; i < to; ++i)
        if (const double d = distSq(in[i], in[from], in[to]); d > worst) worst = d, at = i;
    if (worst <= eps * eps) return;
    keep[at] = true;
    simplify(in, from, at, eps, keep);
    simplify(in, at, to, eps, keep);
}

std::vector<Point> simplified(const std::vector<Point>& in, double eps = 1.0) {
    if (in.size() < 3) return in;
    std::vector<bool> keep(in.size(), false);
    keep.front() = keep.back() = true;
    simplify(in, 0, in.size() - 1, eps, keep);
    std::vector<Point> out;
    for (size_t i = 0; i < in.size(); ++i)
        if (keep[i]) out.push_back(in[i]);
    return out;
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
constexpr const char* kPenModeKey = "penMode";  // "tap": the pen taps instead of writing

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
    penWrites_ = nav.library().metaValue(kPenModeKey) != "tap";
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
    marks_ = nav.library().bookmarks(row_.attachmentKey);
    sendBookmarks();
    if (openAt_) {
        turning_ = true;
        worker_->showAnnotation(*openAt_);
        openAt_.reset();
    }
    sendAnnotations(nav, false);
}

void ReaderScreen::onTick(Nav& nav) {
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
    if (!inkPending_.empty() && !inkDown_ && nav.nowMs() - inkLastMs_ >= kInkCommitMs) commitInk();
    if (!inkPending_.empty() && !inkDown_) nav.wakeAt(inkLastMs_ + kInkCommitMs);  // handwriting saved after a pause
    pumpEvents(nav, true);
    tickResult(nav);
}

void ReaderScreen::tickResult(Nav& nav) {
    if (!result_) return;
    const bool before = result_->shown();
    const PluginResult::Status st = result_->tick(nav);
    if (st == PluginResult::Status::Closed) {  // nothing to show: the selection stays as it was
        result_.reset();
        if (bar_ && bar_->working()) {
            bar_->setWorking(false);
            nav.damage(bar_->rect(), Waveform::Ui);
        }
    } else if (!before && result_->shown() && (selection_ || bar_)) {  // the pop-up replaces the selection
        dropSelection();
        nav.damage(pageArea(), Waveform::Quality);
    }
}

void ReaderScreen::dropSelection() {
    if (result_ && !result_->shown()) {
        result_->stop();
        result_.reset();
    }
    selecting_ = selDragging_ = false;
    selection_.reset();
    bar_.reset();
    note_.reset();
    notePlace_.reset();
}

void ReaderScreen::pumpEvents(Nav& nav, bool visible) {
    if (!worker_) return;
    RenderEvent e;
    while (worker_->poll(e)) {
        switch (e.kind) {
            case RenderEvent::Kind::Opened:
                pageCount_ = e.pageCount;
                views_ = std::max(1, e.viewsPerPage);
                toc_ = e.toc;
                if (!openedSent_) {  // the book really opened (not again after a re-layout)
                    openedSent_ = true;
                    plugins::bookEvent(nav.plugins(), nav.library(), "bookOpened", row_.key, row_.attachmentKey);
                }
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
            case RenderEvent::Kind::Link: {
                if (e.requestId != linkId_) break;
                linkId_ = 0;
                // The reader moved on since the tap: the answer is about a page that's gone.
                if (!e.link || e.page != page_ || turning_ || layoutPending_ || menus_ || editor_ || selecting_) break;
                if (e.link->kind == LinkInfo::Kind::Jump) {
                    followLink(*e.link);
                    break;
                }
                linkPopup_ = e.link;
                if (visible) nav.damage(linkPopupRect(), Waveform::Ui);
                break;
            }
            case RenderEvent::Kind::Placed: {
                if (e.requestId != placeId_) break;
                placeId_ = 0;
                if (e.page < 0) {  // can't be placed here: nothing is sent until the reader reads on
                    unplaced_ = true;
                    break;
                }
                remotePage_ = e.page / std::max(1, e.viewsPerPage);  // a PDF page, whatever the View
                if (remotePage_ == page_ / views_) {  // the page we're on: agreed, nothing to ask
                    nav.library().answerRemotePosition(row_.attachmentKey, true, remoteVersion_);
                    break;
                }
                const int pct = static_cast<int>(std::lround(e.progress * 100));
                std::string text = type_ == DocType::Pdf ? "Your computer is on page " + std::to_string(shownPage(e.page)) + "."
                                                         : "Your computer is at " + std::to_string(pct) + "%" +
                                                               (e.chapter.empty() ? "" : " (" + e.chapter + ")") + ".";
                question_ = PositionQuestion{remotePage_, text + "\nGo there?"};
                if (visible) nav.damage(positionBannerRect(), Waveform::Ui);
                break;
            }
            case RenderEvent::Kind::Ink:
                onInk(e, nav);
                break;
            case RenderEvent::Kind::Area:
                onArea(e, nav);
                break;
            case RenderEvent::Kind::Page: {
                const bool first = image_ == nullptr;
                const bool chapterChanged = !first && e.chapter != chapter_;
                const bool turned = turning_;
                if (e.page != page_ && (selecting_ || selection_ || editor_ || areaDrawing_ || note_)) {  // the page changed under them
                    if (note_ || (editor_ && editor_->wantsKeyboard())) nav.keyboard().hide();
                    editor_.reset();
                    dropSelection();
                }
                if (e.page != page_) dropArea();
                image_ = e.image;
                highlights_ = e.highlights;
                pageBox_ = e.pageBox;
                // Saved ink is drawn by the page once the page carries its annotation.
                inkSaved_.erase(std::remove_if(inkSaved_.begin(), inkSaved_.end(),
                                               [&](const SavedInk& s) {
                                                   if (s.page != e.page) return true;
                                                   for (const auto& b : e.highlights)
                                                       if (b.annotationKey == s.key) return true;
                                                   return false;
                                               }),
                                inkSaved_.end());
                areaSaved_.erase(std::remove_if(areaSaved_.begin(), areaSaved_.end(),
                                                [&](const SavedArea& s) {
                                                    if (s.page != e.page) return true;
                                                    for (const auto& b : e.highlights)
                                                        if (b.annotationKey == s.key) return true;
                                                    return false;
                                                }),
                                 areaSaved_.end());
                page_ = e.page;
                pageCount_ = e.pageCount;
                views_ = std::max(1, e.viewsPerPage);
                chapter_ = e.chapter;
                progress_ = e.progress;
                wordsLeft_ = e.wordsLeftInChapter;
                position_ = e.position;
                turning_ = false;
                layoutPending_ = false;
                nav.library().saveReading(row_.key, position_.value, progress_);
                markPlaces_ = e.bookmarkPlaces;
                links_ = e.links;
                if (turned) linkPopup_.reset();  // a pop-up belongs to the page it opened on
                if (e.landed && arriving_) {  // a jump landed; one onto the page it left needs no Back
                    arriving_ = false;
                    if (!back_.empty() && back_.back().page == page_) back_.pop_back();
                }
                // On Zotero's page, Zotero's exact place (not this page's start) stays the Kindle's.
                if (!e.zoteroPosition.empty())
                    nav.library().setLocalPosition(row_.attachmentKey,
                                                   page_ / views_ == remotePage_ ? remoteValue_ : e.zoteroPosition);
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
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    if (!worker_ || page_ < 0 || layoutPending_) return;  // old numbering until the new pages arrive
    linkId_ = 0;
    commitInk();
    const int target = page_ + delta;
    if (delta > 0 && pageCount_ > 0 && target >= pageCount_ && !finishedSent_) {
        finishedSent_ = true;  // once a reading, however often the last page is swiped on
        plugins::bookEvent(nav.plugins(), nav.library(), "bookFinished", row_.key, row_.attachmentKey);
    }
    if (delta > 0 && pageCount_ > 0 && target >= pageCount_ && !endPrompt_ && !nav.library().isRead(row_.key)) {
        endPrompt_ = true;
        nav.damage(endPromptRect(), Waveform::Ui);
    }
    if (target < 0 || (pageCount_ > 0 && target >= pageCount_)) return;
    if (!back_.empty() && ++turnsSinceJump_ >= kBackTurns) back_.clear();  // read on: no way back
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
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    if (!worker_ || layoutPending_) return;
    linkId_ = 0;
    markJumpOrigin();
    commitInk();
    turning_ = true;
    worker_->show(page);
}

void ReaderScreen::jumpTo(const DocPosition& pos) {
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    menus_.reset();  // we're not visible (a pushed screen called us); the next Page repaints all
    if (!worker_) return;
    linkId_ = 0;
    markJumpOrigin();
    commitInk();
    turning_ = true;
    worker_->showPosition(pos);
}

void ReaderScreen::jumpToAnnotation(const AnnotationRecord& a) {
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    menus_.reset();
    if (!worker_) {
        openAt_ = a;  // opening from Notes: the worker starts in onShow
        return;
    }
    markJumpOrigin();
    commitInk();
    turning_ = true;
    worker_->showAnnotation(a);
}

void ReaderScreen::onResize(Nav& nav) {
    commitInk();  // its points belong to the old layout
    linkPopup_.reset();  // its target is counted in the old layout's pages
    endPrompt_ = false;
    linkId_ = 0;
    fitLayout(layout_, type_, twoColumnsSetting_);
    bar_.reset();  // anchored to the old page
    if (editor_) closeEditor(nav);
    cancelArea(nav);
    if (!worker_) return;
    layoutPending_ = true;
    for (auto& b : back_) b.page = -1;  // turned: pages are counted again
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
    if (linkPopup_ && !(wanted == layout_)) {  // its target is counted in the old layout's pages
        nav.damage(linkPopupRect(), Waveform::Quality);
        linkPopup_.reset();
    }
    linkId_ = 0;
    Layout l = wanted;
    fitLayout(l, type_, twoColumnsSetting_);  // the margin size in pixels, the columns
    if (l == layout_ || !worker_ || !ready()) return;
    layoutPending_ = true;
    layout_ = l;
    for (auto& b : back_) b.page = -1;  // their pages are counted again: Back keeps the place, not the number
    saveLayout(nav.library(), row_.key, l);
    worker_->setLayout(l, position_);
}

void ReaderScreen::openMenus(Nav& nav) {
    linkId_ = 0;
    commitInk();  // leaving the book goes through the menus
    menus_ = std::make_unique<ReaderMenus>(*this, lipc_);
    menus_->open(nav);
}

bool ReaderScreen::dragsAtOnceFrom(Point at) const { return areaArmed_ || (menus_ && menus_->dragsFrom(at)); }

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
    if (endPrompt_) {  // any gesture closes it; its button marks the book read
        const bool mark = g.type == GestureType::Tap && endPromptButtonRect().contains(g.pos);
        nav.damage(endPromptRect(), Waveform::Quality);  // the page under it, without ghosts
        endPrompt_ = false;
        if (mark) {
            nav.library().markRead(row_.key, true);
            plugins::itemMarked(nav.plugins(), nav.library(), row_.key, true);
            nav.sync().pushSoon();
            nav.toast("Marked as read.");
        }
        return;
    }
    if (result_ && result_->shown()) {  // the result pop-up takes every gesture until it is closed
        if (result_->onGesture(g, nav) == PluginResult::Status::Closed) result_.reset();
        return;
    }
    if (linkPopup_) {  // any gesture closes it; its button follows the link
        const LinkInfo info = *linkPopup_;
        const bool go = g.type == GestureType::Tap && linkButtonRect().contains(g.pos);
        nav.damage(linkPopupRect(), Waveform::Quality);  // the page under it, without ghosts
        linkPopup_.reset();
        if (go) followLink(info);
        return;
    }
    if (notePopup_) return onNotePopup(g, nav);
    if (areaDrawing_ && !areaPen_) {  // a finger drawing an area
        switch (g.type) {
            case GestureType::DragStart:
            case GestureType::DragMove:
                return moveArea(g.pos, nav);
            case GestureType::DragEnd:
                return endArea(g.pos, nav);
            case GestureType::LongPressUp:
                return endArea(areaTo_, nav);
            default:
                areaDrawing_ = false;
                nav.damage(pageArea(), Waveform::Quality);
                break;
        }
    }
    if (areaArmed_) {  // Mark an area: the next drag draws one; Cancel ends it; nothing else meanwhile
        if (g.type == GestureType::Tap && areaCancelRect().contains(g.pos)) return cancelArea(nav);
        if (g.type == GestureType::DragStart && ready()) beginArea(g.start, g.pos, nav);
        return;
    }
    if (noteArmed_) {  // Add note: the next tap places it; Cancel ends it; nothing else meanwhile
        if (g.type != GestureType::Tap) return;
        if (areaCancelRect().contains(g.pos)) return cancelArea(nav);
        if (ready() && pageArea().contains(g.pos)) placeNote(g.pos, nav);
        return;
    }
    if (g.type == GestureType::Tap && !editor_ && !note_ && !selecting_ && !selection_ && ready()) {
        if (backLabel() && backRect().contains(g.pos)) return back(nav);
        if (bookmarkRect().contains(g.pos) && !(question_ && positionBannerRect().contains(g.pos)) && !turning_ &&
            !layoutPending_ && !position_.value.empty())
            return toggleBookmark(nav);
        if (!(question_ && positionBannerRect().contains(g.pos)) && tapLink(g.pos, nav)) return;
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
                if (editor_->key() == newAreaKey_ && editor_->bar().colour() != colour_) {  // as a new highlight's
                    colour_ = editor_->bar().colour();
                    nav.library().setMetaValue(kColourKey, colour_);
                }
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
        if (note_->submitted()) {
            if (notePlace_) return saveNote(note_->text(), nav);
            return saveSelection("highlight", note_->text(), nav);
        }
        nav.damage(noteRect(), Waveform::Fast);
        return;
    }
    if (menus_) menus_->onKey(k, nav);
}

bool ReaderScreen::editAt(Point p, Nav& nav) {
    // On a box first; else the nearest box within a fingertip (thin PDF lines are hard to hit).
    // An area is hit near its frame (inside or out), not in its middle, which stays the page's
    // (menus, turns, links, highlights under it); a small area anywhere inside. Nearest wins.
    if (openNoteAt(p, nav)) return true;
    constexpr int kSlack = 30;
    std::string key;
    int best = 1 << 30;
    for (auto it = highlights_.rbegin(); it != highlights_.rend(); ++it) {
        const Rect& r = it->rect;
        const int dx = std::max({r.x - p.x, 0, p.x - (r.x + r.w - 1)});
        const int dy = std::max({r.y - p.y, 0, p.y - (r.y + r.h - 1)});
        int d = std::max(dx, dy);  // 0 inside
        int reach = kSlack;
        if (it->kind == HighlightBox::Kind::Note) continue;  // openNoteAt's
        if (it->kind == HighlightBox::Kind::Image) {
            reach = kAreaFramePx;
            const bool small = r.w < kSmallAreaPx || r.h < kSmallAreaPx;
            if (d == 0 && !small)  // inside: how far from the frame
                d = std::min({p.x - r.x, r.x + r.w - 1 - p.x, p.y - r.y, r.y + r.h - 1 - p.y});
        }
        if (d <= reach && d < best) {
            best = d;
            key = it->annotationKey;
        }
    }
    if (key.empty()) return false;
    auto a = nav.library().annotation(key);
    if (!a) return false;
    Rect anchor;
    for (const auto& h : highlights_)
        if (h.annotationKey == key) anchor = anchor.united(h.rect);
    linkId_ = 0;
    editor_ = std::make_unique<AnnotationEditor>(*a, anchor);
    nav.damage(editor_->rect(), Waveform::Ui);
    if (a->type == "image")  // its frame turns to ink while selected
        for (const Rect& r : frameEdges(anchor, 3)) nav.damage(r.intersected(pageArea()), Waveform::Ui);
    return true;
}

void ReaderScreen::closeEditor(Nav& nav) {
    if (!editor_) return;
    if (editor_->takeChanged()) {
        refreshHighlights(nav);
        nav.sync().pushSoon();
    }
    editor_.reset();
    newAreaKey_.clear();
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
    linkId_ = 0;
    selecting_ = true;
    selDragging_ = selReleased_ = false;
    selAnchor_ = selLast_ = at;
    requestSelection(at);
}

void ReaderScreen::requestSelection(Point to) {
    selLast_ = to;
    if (worker_ && selecting_) selPending_ = worker_->select(page_, selAnchor_, to);
}

void ReaderScreen::onSelection(const RenderEvent& e, Nav& nav) {
    if (!selecting_ || e.requestId != selPending_ || e.page != page_) return;
    // A PDF long press where there's no text (or on a scan) draws an area instead, from where it
    // was pressed to where the finger is now.
    if (type_ == DocType::Pdf && !selection_ && (!e.selection || e.selection->text.empty())) {
        selecting_ = false;
        const bool lifted = selReleased_;
        beginArea(selAnchor_, selLast_, nav);
        if (lifted) endArea(selLast_, nav);
        return;
    }
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
    PluginEntries* e = nav.pluginEntries();
    const bool more = e && !e->actions("selectionAction").empty();
    bar_ = std::make_unique<AnnotationBar>(AnnotationBar::Mode::Create, boundsOf(selection_->rects), colour_, more);
    bar_->setWorking(result_ && result_->running());
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
        case AnnotationBar::Action::More: {
            PluginEntries* e = nav.pluginEntries();
            if (result_) {  // "Working…" tapped: the run stops
                result_->stop();
                result_.reset();
                bar_->setWorking(false);
                nav.damage(bar_->rect(), Waveform::Ui);
                break;
            }
            if (!e) break;
            const Rect before = bar_->rect();
            bar_->showActions(e->actions("selectionAction"));
            nav.damage(before.united(bar_->rect()), Waveform::Ui);
            break;
        }
        case AnnotationBar::Action::Plugin: {
            // The row the user tapped, as the list showed it (plugins may have loaded or restarted
            // since: never send the passage to another plugin than the one picked).
            const auto& listed = bar_->listed();
            if (hit.colour < 0 || hit.colour >= static_cast<int>(listed.size())) break;
            const PluginAction chosen = listed[static_cast<size_t>(hit.colour)];
            PluginEntries* e = nav.pluginEntries();
            bool still = false;
            for (const auto& a : e ? e->actions("selectionAction") : std::vector<PluginAction>{})
                still = still || (a.pluginId == chosen.pluginId && a.id == chosen.id);
            const Rect before = bar_->rect();
            bar_->showActions({});
            nav.damage(before.united(bar_->rect()), before.h > bar_->rect().h ? Waveform::Quality : Waveform::Ui);
            auto item = nav.library().item(row_.key);
            auto att = nav.library().attachment(row_.attachmentKey);
            if (!still) {
                nav.toast(chosen.pluginName + " isn't available right now.");
                break;
            }
            if (!e || !selection_ || !item || !att) break;
            nlohmann::ordered_json target = {{"text", selection_->text},
                                             {"item", plugins::itemJson(*item, nav.library().isRead(row_.key))},
                                             {"attachment", plugins::attachmentJson(*att)},
                                             {"pageLabel", selection_->pageLabel.empty() ? nlohmann::ordered_json(nullptr)
                                                                                         : nlohmann::ordered_json(selection_->pageLabel)}};
            result_ = std::make_unique<PluginResult>(*e, chosen, "selectionAction", std::move(target),
                                                     pageArea());
            bar_->setWorking(true);
            break;
        }
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
    plugins::annotationEvent(nav.plugins(), nav.library(), "highlightCreated", nav.library().createAnnotation(a));
    nav.library().setMetaValue(kColourKey, colour_);
    if (a.localOnly) nav.toast("Saved on the Kindle only — this place can't be synced exactly.");
    else nav.sync().pushSoon();
    refreshHighlights(nav);
    closeSelection(nav);
}

void ReaderScreen::closeSelection(Nav& nav) {
    if (note_) nav.keyboard().hide();
    dropSelection();
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
    if (type_ == DocType::Pdf) return "p. " + std::to_string(shownPage(page_)) + " of " + std::to_string(shownPageCount());
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
        for (const auto& s : inkSaved_)
            if (s.page == page_) strokePaths(cr, s.paths, kInkWidthPx);
        for (const auto& [id, s] : inkSent_)
            if (s.page == page_) strokePaths(cr, s.paths, kInkWidthPx);
        strokePaths(cr, inkPending_, kInkWidthPx);
        if (inkDown_) strokePaths(cr, {inkStroke_}, kInkWidthPx);
        cairo_restore(cr);
    }
    if (selection_) drawSelection(canvas, selection_->rects);
    {  // areas: being drawn (ink), being saved, saved but not yet back with the page
        cairo_save(cr);
        const Rect pa = pageArea();
        cairo_rectangle(cr, pa.x, pa.y, pa.w, pa.h);
        cairo_clip(cr);
        for (const auto& [id, s] : areaSent_)
            if (s.page == page_) strokeFrame(cr, s.rect, areaFrameShade(shadeOf(s.colour)), 3);
        for (const auto& s : areaSaved_)
            if (s.page == page_) strokeFrame(cr, s.rect, areaFrameShade(shadeOf(s.colour)), 3);
        if (auto r = drawnArea()) strokeFrame(cr, *r, kInk, 3);
        if (editor_)  // the area its bar is for: framed in ink while selected
            for (const auto& b : highlights_)
                if (b.kind == HighlightBox::Kind::Image && b.annotationKey == editor_->key()) strokeFrame(cr, b.rect, kInk, 3);
        cairo_restore(cr);
    }
    const Rect f = footerArea();
    if (auto b = backLabel())  // after a jump: the way back, where the chapter line was
        drawText(cr, *b, {kSans, 7, 700}, {kMargin, f.y + 10, W() - 2 * kMargin - 260, 44}, 1);
    else
        drawText(cr, footerLeft(), {kSans, 6.5, 400, false, kTertiary}, {kMargin, f.y + 12, W() - 2 * kMargin - 260, 40}, 1);
    if (bookmarked()) {  // a ribbon hanging from the top-right corner
        const double x = W() - 110, w = 54, h = 110;
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x + w, 0);
        cairo_line_to(cr, x + w, h);
        cairo_line_to(cr, x + w / 2, h - 22);
        cairo_line_to(cr, x, h);
        cairo_close_path(cr);
        gray(cr, kInk);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 3);  // a white edge: it shows on dark scans too
        gray(cr, 255);
        cairo_stroke(cr);
    }
    drawText(cr, footerRight(), {kSans, 6.5, 400, false, kTertiary}, {W() - kMargin - 260, f.y + 12, 260, 40}, 1, Align::Right);
    if (bar_) bar_->draw(cr);
    if (notePopup_) notePopup_->draw(cr);
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
    if (((areaArmed_ && !areaDrawing_) || noteArmed_) && !menus_) {
        const Rect b = areaBannerRect();
        roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, 14);
        gray(cr, 255);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 3);
        gray(cr, kInk);
        cairo_stroke(cr);
        const Rect cancel = areaCancelRect();
        centredText(cr, noteArmed_ ? "Tap where the note goes." : "Drag across the area to mark.", {kSans, 8, 500}, {b.x + 36, b.y, cancel.x - b.x - 60, b.h}, Align::Left);
        drawButton(cr, cancel, "Cancel", false);
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
    if (endPrompt_ && !menus_) {
        const Rect p = endPromptRect();
        fillRect(cr, p, 255);
        fillRect(cr, {p.x, p.y, p.w, 3}, kInk);
        drawText(cr, "The end", {kSans, 7, 500, false, kSecondary}, {kMargin, p.y + 34, p.w - 2 * kMargin, 44}, 1);
        drawText(cr, row_.title.empty() ? "Untitled" : row_.title, {kSerif, 9, 700},
                 {kMargin, p.y + 88, p.w - 2 * kMargin, 60}, 1);
        drawButton(cr, endPromptButtonRect(), "Mark as read", true);
    }
    if (linkPopup_ && !menus_) {
        const Rect p = linkPopupRect();
        fillRect(cr, p, 255);
        fillRect(cr, {p.x, p.y, p.w, 3}, kInk);
        const int w = p.w - 2 * kMargin;
        const LinkInfo& l = *linkPopup_;
        drawText(cr, l.label, {kSans, 7, 500, false, kSecondary}, {kMargin, p.y + 34, w, 44}, 1);
        const Rect button = linkButtonRect();
        const int textBottom = (button.empty() ? p.y + p.h - 30 : button.y - 30);
        if (l.kind == LinkInfo::Kind::Web)
            drawText(cr, l.url, {kSans, 8.5, 500}, {kMargin, p.y + 88, w, textBottom - p.y - 88}, 4);
        else
            drawText(cr, l.text, {kSerif, 8.5, 400}, {kMargin, p.y + 88, w, textBottom - p.y - 88}, 12);
        if (!button.empty())
            drawButton(cr, button, linkButtonLabel(), false);
    }
    if (result_ && !menus_) result_->draw(cr);
    if (menus_) menus_->draw(canvas);
}

// --- Getting around: Back, bookmarks, Go to ---------------------------------------------------

void ReaderScreen::markJumpOrigin() {
    turnsSinceJump_ = 0;
    if (page_ < 0 || position_.value.empty() || layoutPending_) return;
    arriving_ = true;
    if (!back_.empty() && back_.back().page == page_ && page_ >= 0) return;
    back_.push_back({position_, page_});
}

std::optional<std::string> ReaderScreen::backLabel() const {
    if (back_.empty()) return std::nullopt;
    // The page number only while the layout it was counted in lasts.
    std::string label = back_.back().page >= 0 ? "‹ Back to page " + std::to_string(shownPage(back_.back().page)) : "‹ Back";
    if (!chapter_.empty()) label += " · " + chapter_;  // where you are now, as the footer said
    return label;
}

Rect ReaderScreen::backRect() { return {0, H() - kFooterH, W() * 3 / 5, kFooterH}; }

void ReaderScreen::back(Nav& nav) {
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    if (turning_ || back_.empty()) return;
    const BackEntry to = back_.back();
    back_.pop_back();
    turnsSinceJump_ = 0;
    commitInk();
    turning_ = true;
    // The page (the view, in PDF column view) while its layout lasts; else the position.
    if (to.page >= 0) worker_->show(to.page);
    else worker_->showPosition(to.pos);
    nav.damage(footerArea(), Waveform::Ui);
}

Rect ReaderScreen::bookmarkRect() { return {W() - 150, 0, 150, 160}; }

int ReaderScreen::markPage(const std::string& position) const {
    for (const auto& [pos, page] : markPlaces_)
        if (pos == position) return page;
    return -1;
}

bool ReaderScreen::bookmarked() const {
    if (page_ < 0) return false;
    for (const auto& m : marks_)  // a page's mark, on all its views (PDF column view)
        if (const int p = markPage(m.position); p >= 0 && p / views_ == page_ / views_) return true;
    return false;
}

std::vector<TocEntry> ReaderScreen::bookmarks() const {
    std::vector<TocEntry> out;
    for (const auto& m : marks_) {
        const int page = markPage(m.position);
        if (page < 0) continue;
        // The page number shows beside it, as in the chapter list.
        out.push_back({m.chapter.empty() ? "Bookmark" : m.chapter, 0, {m.position}, page});
    }
    std::stable_sort(out.begin(), out.end(), [](const TocEntry& a, const TocEntry& b) { return a.page < b.page; });
    return out;
}

void ReaderScreen::sendBookmarks() {
    if (!worker_) return;
    std::vector<DocPosition> positions;
    for (const auto& m : marks_) positions.push_back({m.position});
    worker_->setBookmarks(std::move(positions));
}

void ReaderScreen::toggleBookmark(Nav& nav) {
    if (bookmarked()) {
        for (const auto& m : marks_)
            if (const int p = markPage(m.position); p >= 0 && p / views_ == page_ / views_)
                nav.library().removeBookmark(row_.attachmentKey, m.position);
    } else {
        nav.library().addBookmark(row_.attachmentKey, {position_.value, chapter_, nav.nowMs()});
    }
    marks_ = nav.library().bookmarks(row_.attachmentKey);
    sendBookmarks();  // the page comes again, with or without its ribbon
}

void ReaderScreen::goToPage(int page) {
    menus_.reset();
    turnTo(page * views_);  // a PDF page: its first view
}

int ReaderScreen::shownPage(int page) const { return page / views_ + 1; }

void ReaderScreen::jumpToHit(const SearchHit& h) {
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    // A PDF hit knows its view (column view); its position only the page.
    if (type_ != DocType::Pdf || h.page < 0 || (pageCount_ > 0 && h.page >= pageCount_)) return jumpTo(h.pos);
    menus_.reset();
    if (!worker_) return;
    linkId_ = 0;
    markJumpOrigin();
    commitInk();
    turning_ = true;
    worker_->show(h.page);
}
int ReaderScreen::shownPageCount() const { return (pageCount_ + views_ - 1) / views_; }

// --- Links ---------------------------------------------------------------------------------

bool ReaderScreen::tapLink(Point p, Nav&) {
    if (turning_ || layoutPending_ || links_.empty() || !worker_) return false;
    const Rect page = pageArea();
    const int strip = page.w * 8 / 100;  // the outer strips always turn the page
    if (p.x < page.x + strip || p.x >= page.x + page.w - strip) return false;
    constexpr int kSlack = 14;   // fingers are wider than a note mark
    constexpr int kTarget = 96;  // ~8 mm: a dagger a few pixels wide is still hit by a fingertip
    const LinkBox* best = nullptr;
    int bestDistance = 0;
    for (const LinkBox& b : links_) {
        const int w = std::max(b.rect.w + 2 * kSlack, kTarget), h = std::max(b.rect.h + 2 * kSlack, kTarget);
        const Point c = b.rect.center();
        const Rect r{c.x - w / 2, c.y - h / 2, w, h};
        if (!r.contains(p)) continue;
        const int d = std::abs(c.x - p.x) + std::abs(c.y - p.y);
        if (!best || d < bestDistance) {
            best = &b;
            bestDistance = d;
        }
    }
    if (!best) return false;
    linkId_ = worker_->linkAt(page_, best->index);
    return true;
}

void ReaderScreen::followLink(const LinkInfo& l) {
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    if (!worker_ || (l.target.value.empty() && l.targetPage < 0)) return;
    markJumpOrigin();
    commitInk();
    turning_ = true;
    if (!l.target.value.empty()) worker_->showPosition(l.target);
    else worker_->show(l.targetPage);
}

Rect ReaderScreen::linkPopupRect() const {
    if (!linkPopup_) return {};
    const Rect page = pageArea();
    const int w = page.w - 2 * kMargin;
    const std::string& text = linkPopup_->kind == LinkInfo::Kind::Web ? linkPopup_->url : linkPopup_->text;
    const int textH = measureText(text, {kSerif, 8.5, 400}, w, 12).height;
    const bool button = linkPopup_->kind != LinkInfo::Kind::Web && linkPopup_->targetPage >= 0;
    const int h = std::min(page.h * 11 / 20, 40 + 54 + textH + (button ? 40 + 90 : 0) + 40);
    return {0, page.y + page.h - h, page.w, h};
}

std::string ReaderScreen::linkButtonLabel() const {
    if (!linkPopup_) return "";
    return linkPopup_->kind == LinkInfo::Kind::Note ? "Go to note" : "Go to page " + std::to_string(shownPage(linkPopup_->targetPage));
}

Rect ReaderScreen::endPromptRect() const {
    if (!endPrompt_) return {};
    const Rect page = pageArea();
    const int h = 40 + 54 + 60 + 40 + 90 + 40;
    return {0, page.y + page.h - h, page.w, h};
}

Rect ReaderScreen::endPromptButtonRect() const {
    if (!endPrompt_) return {};
    const Rect p = endPromptRect();
    return {kMargin, p.y + p.h - 40 - 90, 420, 90};
}

Rect ReaderScreen::linkButtonRect() const {
    if (!linkPopup_ || linkPopup_->kind == LinkInfo::Kind::Web || linkPopup_->targetPage < 0) return {};
    const Rect p = linkPopupRect();
    return {kMargin, p.y + p.h - 40 - 90, 420, 90};
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
        dropArea();
        markJumpOrigin();
        turning_ = true;
        worker_->show(question_->page * views_);  // its first view (PDF column view)
    }
    question_.reset();
    nav.damage(positionBannerRect(), Waveform::Ui);
}

// --- Areas ---------------------------------------------------------------------------------

std::optional<Rect> ReaderScreen::drawnArea() const {
    if (!areaDrawing_) return std::nullopt;
    const Rect r{std::min(areaFrom_.x, areaTo_.x), std::min(areaFrom_.y, areaTo_.y), std::abs(areaTo_.x - areaFrom_.x),
                 std::abs(areaTo_.y - areaFrom_.y)};
    return r.intersected(pageBox_.empty() ? pageArea() : pageBox_.intersected(pageArea()));
}

Rect ReaderScreen::areaBannerRect() { return {kMargin / 2, 30, W() - kMargin, 150}; }
Rect ReaderScreen::areaCancelRect() {
    const Rect b = areaBannerRect();
    return {b.x + b.w - 30 - 220, b.y + (b.h - 96) / 2, 220, 96};
}

void ReaderScreen::markArea(Nav& nav) {
    closeMenus(nav);
    if (type_ != DocType::Pdf || !ready()) return;
    linkId_ = 0;
    noteArmed_ = false;
    areaArmed_ = true;
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
    nav.damage(areaBannerRect(), Waveform::Ui);
}

void ReaderScreen::beginArea(Point from, Point to, Nav& nav) {
    linkId_ = 0;
    areaDrawing_ = true;
    areaFrom_ = from;
    areaTo_ = to;
    if (areaArmed_) nav.damage(areaBannerRect(), Waveform::Fast);  // the banner gives way to the drawing
    for (const Rect& r : frameEdges(*drawnArea(), 3)) nav.damage(r.intersected(pageArea()), Waveform::Fast);
}

void ReaderScreen::moveArea(Point to, Nav& nav) {
    if (!areaDrawing_) return;
    const Rect before = *drawnArea();
    areaTo_ = to;
    const Rect now = *drawnArea();
    if (now == before) return;
    for (const Rect& r : frameEdges(before, 3)) nav.damage(r.intersected(pageArea()), Waveform::Fast);
    for (const Rect& r : frameEdges(now, 3)) nav.damage(r.intersected(pageArea()), Waveform::Fast);
}

void ReaderScreen::endArea(Point to, Nav& nav) {
    if (!areaDrawing_) return;
    moveArea(to, nav);
    const Rect r = *drawnArea();
    areaDrawing_ = areaPen_ = false;
    if (r.w < kMinAreaPx || r.h < kMinAreaPx || !worker_ || page_ < 0) {  // a slip: nothing (Mark an area waits on)
        for (const Rect& e : frameEdges(r, 3)) nav.damage(e.intersected(pageArea()), Waveform::Ui);
        if (areaArmed_) nav.damage(areaBannerRect(), Waveform::Ui);
        return;
    }
    if (areaArmed_) {
        areaArmed_ = false;
        nav.damage(areaBannerRect(), Waveform::Quality);
    }
    areaSent_[worker_->area(page_, r)] = SentArea{page_, r, colour_};  // framed meanwhile
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
}

void ReaderScreen::dropArea() {
    areaArmed_ = areaDrawing_ = areaPen_ = noteArmed_ = false;
    notePopup_.reset();
    if (selecting_ && !selection_) selecting_ = false;  // a long press still waiting can't start one later
}

void ReaderScreen::cancelArea(Nav& nav) {
    if (areaDrawing_) nav.damage(pageArea(), Waveform::Quality);
    else if (areaArmed_ || noteArmed_) nav.damage(areaBannerRect(), Waveform::Quality);
    if (notePopup_) nav.damage(notePopup_->rect(), Waveform::Quality);
    notePopup_.reset();
    areaDrawing_ = areaPen_ = areaArmed_ = noteArmed_ = false;
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
}

void ReaderScreen::onArea(const RenderEvent& e, Nav& nav) {
    if (auto n = noteSent_.find(e.requestId); n != noteSent_.end()) {
        const SentNote sent = n->second;
        noteSent_.erase(n);
        if (!e.ink) {
            nav.toast("This note couldn't be saved.");
            return;
        }
        AnnotationRecord a;
        a.parentKey = row_.attachmentKey;
        a.type = "note";
        a.comment = sent.text;
        a.color = sent.colour;
        a.position = e.ink->position;
        a.sortIndex = e.ink->sortIndex;
        a.pageLabel = e.ink->pageLabel;
        plugins::annotationEvent(nav.plugins(), nav.library(), "highlightCreated", nav.library().createAnnotation(a));
        nav.sync().pushSoon();
        refreshHighlights(nav);
        return;
    }
    auto it = areaSent_.find(e.requestId);
    if (it == areaSent_.end()) return;
    const SentArea sent = it->second;
    areaSent_.erase(it);
    if (!e.ink) {
        nav.toast("This area couldn't be saved.");
        nav.damage(pageArea(), Waveform::Quality);
        return;
    }
    AnnotationRecord a;
    a.parentKey = row_.attachmentKey;
    a.type = "image";
    a.color = sent.colour;
    a.position = e.ink->position;
    a.sortIndex = e.ink->sortIndex;
    a.pageLabel = e.ink->pageLabel;
    const std::string key = nav.library().createAnnotation(a);
    plugins::annotationEvent(nav.plugins(), nav.library(), "highlightCreated", key);
    nav.library().setMetaValue(kColourKey, sent.colour);
    nav.sync().pushSoon();
    areaSaved_.push_back(SavedArea{key, sent.page, sent.rect, sent.colour});
    refreshHighlights(nav);
    // Its bar, as a new highlight gets one: unless the reader moved on meanwhile.
    if (sent.page != page_ || editor_ || menus_ || selecting_ || selection_ || note_ || areaDrawing_ || areaArmed_ ||
        noteArmed_ || notePopup_ ||
        turning_ || (result_ && result_->shown()))
        return;
    auto saved = nav.library().annotation(key);
    if (!saved) return;
    editor_ = std::make_unique<AnnotationEditor>(*saved, sent.rect);
    newAreaKey_ = key;
    nav.damage(editor_->rect(), Waveform::Ui);
}

// --- Sticky notes ----------------------------------------------------------------------------

void ReaderScreen::addNote(Nav& nav) {
    closeMenus(nav);
    if (type_ != DocType::Pdf || !ready()) return;
    linkId_ = 0;
    areaArmed_ = false;
    noteArmed_ = true;
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
    nav.damage(areaBannerRect(), Waveform::Ui);
}

void ReaderScreen::placeNote(Point at, Nav& nav) {
    noteArmed_ = false;
    nav.damage(areaBannerRect(), Waveform::Quality);
    if (!worker_ || page_ < 0) return;
    linkId_ = 0;
    notePlace_ = NotePlace{page_, at};
    note_.emplace();
    nav.keyboard().show();
    noteBox_ = AnnotationEditor::commentRectAbove(nav.keyboard().bounds());
    nav.damage(noteRect(), Waveform::Ui);
}

void ReaderScreen::saveNote(const std::string& text, Nav& nav) {
    const std::optional<NotePlace> place = notePlace_;
    closeSelection(nav);
    if (!place || !worker_ || text.find_first_not_of(" \t\n") == std::string::npos) return;  // empty: no note
    noteSent_[worker_->note(place->page, place->at)] = SentNote{text, colour_};
    nav.library().setMetaValue(kColourKey, colour_);
}

bool ReaderScreen::openNoteAt(Point p, Nav& nav) {
    // The nearest note whose icon is within ~9 mm of the tap.
    const HighlightBox* best = nullptr;
    int bestD = kNoteReachPx + 1;
    for (const auto& b : highlights_) {
        if (b.kind != HighlightBox::Kind::Note) continue;
        const Rect r = noteIcon(b.rect);
        const int dx = std::max({r.x - p.x, 0, p.x - (r.x + r.w - 1)});
        const int dy = std::max({r.y - p.y, 0, p.y - (r.y + r.h - 1)});
        if (std::max(dx, dy) < bestD) {
            bestD = std::max(dx, dy);
            best = &b;
        }
    }
    if (!best) return false;
    auto a = nav.library().annotation(best->annotationKey);
    if (!a) return false;
    linkId_ = 0;
    notePopup_ = std::make_unique<NotePopup>(*a, noteIcon(best->rect), pageArea());
    nav.damage(notePopup_->rect(), Waveform::Ui);
    return true;
}

void ReaderScreen::onNotePopup(const Gesture& g, Nav& nav) {
    const NotePopup::Action act = notePopup_->onGesture(g, nav);
    if (act == NotePopup::Action::None) return;
    const std::string key = notePopup_->key();
    const Rect icon = notePopup_->icon();
    nav.damage(notePopup_->rect(), Waveform::Quality);  // the page under it, without ghosts
    notePopup_.reset();
    if (act == NotePopup::Action::Close) return;
    auto a = nav.library().annotation(key);
    if (!a || a->deletedHere) return;
    editor_ = std::make_unique<AnnotationEditor>(*a, icon);
    if (act == NotePopup::Action::Edit) editor_->editComment(nav);
    else editor_->confirmDelete(nav);
}

// --- Handwriting -----------------------------------------------------------------------------

bool ReaderScreen::penAllowed() const {
    return worker_ && image_ && !error_ && !menus_ && !selecting_ && !selection_ && !editor_ && !note_ &&
           !areaArmed_ && !areaDrawing_ && !noteArmed_ && !notePopup_ &&
           !layoutPending_ && !turning_ && !linkPopup_ && !(result_ && result_->shown());
}

bool ReaderScreen::wantsSystemInk() const {
    // Not in landscape yet: the handler's canvas is registered portrait (untried turned).
    return type_ == DocType::Pdf && penWrites_ && penAllowed() && !eraserNear_ && !buttonHeld_ && !landscape();
}

void ReaderScreen::setPenWrites(bool on, Nav& nav) {
    penWrites_ = on;
    nav.library().setMetaValue(kPenModeKey, on ? "" : "tap");
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
}

bool ReaderScreen::onPen(const PenEvent& p, Nav& nav) {
    penSeen_ = true;
    const bool eraserNear = p.eraser && p.type != PenEvent::Type::Leave;
    const bool buttonHeld = p.button && p.type != PenEvent::Type::Leave;
    if (eraserNear != eraserNear_ || buttonHeld != buttonHeld_) {
        eraserNear_ = eraserNear;
        buttonHeld_ = buttonHeld;
        if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
    }
    const bool lift = p.type == PenEvent::Type::Up || p.type == PenEvent::Type::Leave;
    if (areaDrawing_ && areaPen_) {  // an area drawn with the pen
        if (!lift) {
            if (p.type == PenEvent::Type::Move) moveArea(p.pos, nav);
            return true;
        }
        // A touch that hardly moved isn't an area: a tap, or held, a long press (text selection).
        const int dx = std::abs(p.pos.x - areaFrom_.x), dy = std::abs(p.pos.y - areaFrom_.y);
        if (dx <= 3 * kTapSlopPx && dy <= 3 * kTapSlopPx) {
            const Point at = areaFrom_;
            const bool held = nav.nowMs() - areaDownMs_ >= GestureConfig{}.longpress_ms;
            areaDrawing_ = areaPen_ = false;
            for (const Rect& r : frameEdges(Rect{std::min(at.x, p.pos.x), std::min(at.y, p.pos.y), dx + 1, dy + 1}, 3))
                nav.damage(r.intersected(pageArea()), Waveform::Ui);
            if (held) {
                onGesture(Gesture{GestureType::LongPress, at, at}, nav);
                onGesture(Gesture{GestureType::LongPressUp, at, at}, nav);
            } else {
                onGesture(Gesture{GestureType::Tap, at, at}, nav);
            }
            return false;
        }
        endArea(p.pos, nav);
        return false;
    }
    if (lift && wipeFrom_) {
        nav.damage(footerArea().contains(*wipeFrom_) ? footerArea() : Rect{0, 0, W(), H()}, Waveform::Ui);
        wipeFrom_.reset();
    }
    if (lift) {
        if (inkDown_) {
            inkDown_ = false;
            // A quick touch that hardly moved, with nothing written before it, is a tap (menus,
            // page turns), not a dot: a dot right after writing (an i, a full stop) stays ink.
            Rect extent;
            for (const Point& q : inkStroke_) extent = extent.united({q.x, q.y, 1, 1});
            const bool tap = inkPending_.empty() && !inkStroke_.empty() && extent.w <= kTapSlopPx &&
                             extent.h <= kTapSlopPx && nav.nowMs() - inkDownMs_ <= kTapMs;
            if (tap) {
                const Point at = inkStroke_.front();
                inkStroke_.clear();
                const int pad = kInkWidthPx + 2;  // the system may have inked the dot
                nav.damage({extent.x - pad, extent.y - pad, extent.w + 2 * pad, extent.h + 2 * pad}, Waveform::Ui);
                onGesture(Gesture{GestureType::Tap, at, at}, nav);
                return false;
            }
            if (!inkStroke_.empty()) {
                if (inkPending_.empty()) inkFirstMs_ = inkDownMs_;
                inkPending_.push_back(simplified(inkStroke_));
            }
            inkStroke_.clear();
            inkLastMs_ = nav.nowMs();
            size_t points = 0;
            for (const auto& s : inkPending_) points += s.size();
            if (inkPending_.size() >= kMaxInkStrokes || points >= kMaxInkPoints ||
                inkLastMs_ - inkFirstMs_ >= kMaxInkSpanMs)
                commitInk();
        }
        if (erasing_) {
            erasing_ = false;
            eraseAlong(eraserPath_, nav);
            eraserPath_.clear();
        }
        return false;
    }
    if (p.type == PenEvent::Type::Near) return false;
    if (p.type == PenEvent::Type::Down && type_ == DocType::Pdf && pageArea().contains(p.pos) &&
        !(question_ && positionBannerRect().contains(p.pos))) {
        // Mark an area on: the pen draws the area (its Cancel is tapped as with a finger). With the
        // side button held the pen is a finger, which draws one the same way.
        if (areaArmed_ && !p.button && worker_ && image_ && !menus_ && !areaCancelRect().contains(p.pos)) {
            areaPen_ = true;
            areaDownMs_ = nav.nowMs();
            beginArea(p.pos, p.pos, nav);
            return true;
        }
    }
    if (p.type == PenEvent::Type::Down) {
        // EPUB (Zotero has no ink there), menus, selection, the footer, the reading-position
        // question's banner, the pen set to tap or its side button held: the pen is a finger.
        const bool onBanner = question_ && positionBannerRect().contains(p.pos);
        if (type_ != DocType::Pdf || !penWrites_ || p.button || !penAllowed() || !pageArea().contains(p.pos) ||
            onBanner) {
            if (nav.systemInk()) wipeFrom_ = p.pos;  // the system may have inked it: repaint on lift
            return false;
        }
        if (p.eraser) {
            erasing_ = true;
            eraserPath_ = {p.pos};
            return true;
        }
        if (inkPage_ != page_) commitInk();
        inkPage_ = page_;
        inkDown_ = true;
        inkDownMs_ = nav.nowMs();
        inkStroke_ = {p.pos};
        const Rect r{p.pos.x - kInkWidthPx, p.pos.y - kInkWidthPx, 2 * kInkWidthPx + 1, 2 * kInkWidthPx + 1};
        if (!nav.systemInk())  // the system's wet ink already shows the line
            nav.paintNow(r.intersected(pageArea()), Waveform::Ink,
                     [&](cairo_t* cr) { strokePaths(cr, {{p.pos}}, kInkWidthPx); });
        return true;
    }
    // Move.
    if (erasing_) {
        if (eraserPath_.empty() || std::abs(eraserPath_.back().x - p.pos.x) + std::abs(eraserPath_.back().y - p.pos.y) >= 4)
            eraserPath_.push_back(p.pos);
        return true;
    }
    if (!inkDown_ || inkStroke_.empty()) return false;
    const Point a = inkStroke_.back(), b = p.pos;
    if (std::abs(a.x - b.x) + std::abs(a.y - b.y) < 2) return true;
    inkStroke_.push_back(b);
    if (nav.systemInk()) return true;
    const int pad = kInkWidthPx + 1;
    const Rect r{std::min(a.x, b.x) - pad, std::min(a.y, b.y) - pad, std::abs(a.x - b.x) + 2 * pad + 1,
                 std::abs(a.y - b.y) + 2 * pad + 1};
    nav.paintNow(r.intersected(pageArea()), Waveform::Ink,
                 [&](cairo_t* cr) { strokePaths(cr, {{a, b}}, kInkWidthPx); });
    return true;
}

void ReaderScreen::commitInk() {
    if (inkPending_.empty() || !worker_ || inkPage_ < 0) return;
    const uint64_t id = worker_->ink(inkPage_, inkPending_, kInkWidthPx);
    inkSent_[id] = SentInk{inkPage_, std::move(inkPending_), false};
    inkPending_.clear();
}

void ReaderScreen::flushInk(Nav& nav, int64_t timeoutMs) {
    commitInk();
    if (inkFlushGaveUp_) timeoutMs = 0;
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    pumpEvents(nav, false);
    while (!inkSent_.empty() && clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pumpEvents(nav, false);  // the Ink answers make the annotations
    }
    if (!inkSent_.empty()) {
        inkFlushGaveUp_ = true;
        log(LogLevel::Warn, "reader: handwriting not placed in time, %zu request(s) lost", inkSent_.size());
    }
}

void ReaderScreen::onInk(const RenderEvent& e, Nav& nav) {
    auto it = inkSent_.find(e.requestId);
    if (it == inkSent_.end()) return;
    SentInk sent = std::move(it->second);
    inkSent_.erase(it);
    if (sent.paths.empty()) return;  // all of it erased while it was being placed
    if (sent.erased) {               // placed with strokes since erased: place what is left
        const uint64_t id = worker_->ink(sent.page, sent.paths, kInkWidthPx);
        inkSent_[id] = SentInk{sent.page, std::move(sent.paths), false};
        return;
    }
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
    const std::string key = nav.library().createAnnotation(a);
    plugins::annotationEvent(nav.plugins(), nav.library(), "highlightCreated", key);
    nav.sync().pushSoon();
    inkSaved_.push_back(SavedInk{key, sent.page, std::move(sent.paths)});
    refreshHighlights(nav);
}

// The eraser end takes away the strokes it crosses, one by one: strokes not saved yet at once;
// a saved ink annotation loses those strokes (rewritten without them), and is deleted when none
// is left, unless it carries a comment or tags (then Notes is the place to delete it).
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
    auto near = [&](const std::vector<Point>& p, double r) { return pathNear(p, probe, r); };
    bool changed = false;

    const size_t before = inkPending_.size();
    inkPending_.erase(std::remove_if(inkPending_.begin(), inkPending_.end(), [&](const auto& p) { return near(p, radius); }),
                      inkPending_.end());
    changed |= inkPending_.size() != before;
    for (auto& [id, sent] : inkSent_) {
        const size_t n = sent.paths.size();
        sent.paths.erase(std::remove_if(sent.paths.begin(), sent.paths.end(), [&](const auto& p) { return near(p, radius); }),
                         sent.paths.end());
        if (sent.paths.size() != n) sent.erased = changed = true;
    }

    // Saved annotations: which of their strokes the eraser crossed (by stroke index, the order
    // of annotationPosition.paths). The page's drawing if it has them yet, else the strokes kept
    // since saving.
    std::map<std::string, std::pair<std::set<size_t>, size_t>> hits;  // key -> (crossed, strokes)
    std::set<std::string> onPage;
    for (const auto& b : highlights_) {
        if (b.kind != HighlightBox::Kind::Ink) continue;
        onPage.insert(b.annotationKey);
        for (size_t i = 0; i < b.paths.size(); ++i)
            if (near(b.paths[i], b.strokePx / 2.0 + 12)) {
                auto& h = hits[b.annotationKey];
                h.first.insert(i);
                h.second = b.paths.size();
            }
    }
    for (const auto& s : inkSaved_) {
        if (s.page != page_ || onPage.count(s.key)) continue;
        for (size_t i = 0; i < s.paths.size(); ++i)
            if (near(s.paths[i], radius)) {
                auto& h = hits[s.key];
                h.first.insert(i);
                h.second = s.paths.size();
            }
    }
    bool saved = false;
    for (const auto& [key, h] : hits) {
        auto rec = nav.library().annotation(key);
        if (!rec || rec->deletedHere) continue;
        const auto& [crossed, strokes] = h;
        if (crossed.size() < strokes) {
            nlohmann::json pos = nlohmann::json::parse(rec->position, nullptr, false);
            if (pos.is_discarded() || !pos.contains("paths") || pos["paths"].size() != strokes) continue;
            nlohmann::json kept = nlohmann::json::array();
            for (size_t i = 0; i < strokes; ++i)
                if (!crossed.count(i)) kept.push_back(pos["paths"][i]);
            pos["paths"] = kept;
            rec->position = pos.dump();
            nav.library().editAnnotation(*rec);
            plugins::annotationEvent(nav.plugins(), nav.library(), "highlightChanged", key);
        } else if (!rec->comment.empty() || !rec->tags.empty()) {
            nav.toast("This handwriting has a comment or tags: delete it in Notes.");
            continue;
        } else {
            const bool told = nav.plugins().listening();
            const plugins::AnnotationRef ref = told ? plugins::annotationRef(nav.library(), *rec) : plugins::AnnotationRef{};
            nav.library().deleteAnnotation(key);
            if (told) plugins::annotationDeleted(nav.plugins(), ref);
        }
        saved = true;
        // Shown at once: those strokes off the page's drawing and the kept strokes.
        for (auto& b : highlights_)
            if (b.annotationKey == key) {
                std::vector<std::vector<Point>> left;
                for (size_t i = 0; i < b.paths.size(); ++i)
                    if (!crossed.count(i)) left.push_back(b.paths[i]);
                b.paths = std::move(left);
            }
        for (auto& s : inkSaved_)
            if (s.key == key) {
                std::vector<std::vector<Point>> left;
                for (size_t i = 0; i < s.paths.size(); ++i)
                    if (!crossed.count(i)) left.push_back(s.paths[i]);
                s.paths = std::move(left);
            }
    }
    if (saved) {
        highlights_.erase(std::remove_if(highlights_.begin(), highlights_.end(),
                                         [](const HighlightBox& b) { return b.kind == HighlightBox::Kind::Ink && b.paths.empty(); }),
                          highlights_.end());
        nav.sync().pushSoon();
        refreshHighlights(nav);
        changed = true;
    }
    if (changed) nav.damage(pageArea(), Waveform::Quality);
}

}  // namespace zk
