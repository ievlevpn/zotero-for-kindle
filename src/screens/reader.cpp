#include "screens/reader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "app/settings_store.h"
#include "core/log.h"
#include "screens/annotation_bar.h"
#include "screens/book_search.h"
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

// The page's pixels into the canvas, inside its paint region only.
void blit(Canvas& c, const PageImage& img, int y0) {
    const Rect r = Rect{0, y0, img.width, img.height}.intersected(c.paintRegion());
    if (r.empty()) return;
    cairo_surface_flush(c.surface());
    unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (int cy = r.y; cy < r.y + r.h; ++cy) {
        auto* row = reinterpret_cast<uint32_t*>(data + cy * stride) + r.x;
        const size_t y = static_cast<size_t>(cy - y0);
        if (!img.rgb.empty()) {  // a color page (a color screen)
            const uint8_t* src = img.rgb.data() + (y * img.width + r.x) * 3;
            for (int x = 0; x < r.w; ++x, src += 3) row[x] = 0xff000000u | (src[0] << 16) | (src[1] << 8) | src[2];
            continue;
        }
        const uint8_t* src = img.gray.data() + y * img.width + r.x;
        for (int x = 0; x < r.w; ++x) row[x] = 0xff000000u | (src[x] * 0x010101u);
    }
    cairo_surface_mark_dirty_rectangle(c.surface(), r.x, r.y, r.w, r.h);
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

// Where a highlight box draws (a text box's words and a note's icon may reach past its rect), with
// a little room for antialiasing.
Rect drawnRect(const HighlightBox& b) {
    Rect r = b.rect;
    if (b.kind == HighlightBox::Kind::Text) r = {r.x, r.y, std::max(r.w, 40), std::max(r.h, b.fontPx + 8)};
    if (b.kind == HighlightBox::Kind::Note) r = r.united(noteIcon(b.rect));
    return {r.x - 6, r.y - 6, r.w + 12, r.h + 12};
}

bool sameBox(const HighlightBox& a, const HighlightBox& b) {
    return a.annotationKey == b.annotationKey && a.rect == b.rect && a.shade == b.shade && a.underline == b.underline &&
           a.kind == b.kind && a.text == b.text && a.fontPx == b.fontPx && a.paths == b.paths && a.strokePx == b.strokePx &&
           a.color == b.color;
}

// What changed between two sets of highlight boxes: the boxes only one of them has, where they draw.
std::vector<Rect> changedBoxes(const std::vector<HighlightBox>& before, const std::vector<HighlightBox>& after) {
    std::vector<Rect> out;
    auto missing = [&](const std::vector<HighlightBox>& from, const std::vector<HighlightBox>& in) {
        for (const auto& b : from)
            if (std::none_of(in.begin(), in.end(), [&](const HighlightBox& o) { return sameBox(b, o); })) out.push_back(drawnRect(b));
    };
    missing(before, after);
    missing(after, before);
    return out;
}

// A colour (0xRRGGBB) as cairo's source.
void rgb(cairo_t* cr, uint32_t c) {
    cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

void strokeFrame(cairo_t* cr, Rect r, uint8_t shade, int w, std::optional<uint32_t> colour = std::nullopt) {
    if (colour) rgb(cr, *colour);
    else gray(cr, shade);
    cairo_set_line_width(cr, w);
    cairo_rectangle(cr, r.x + w / 2.0, r.y + w / 2.0, r.w - w, r.h - w);
    cairo_stroke(cr);
}

// The pixels of `r` multiplied by `tint` (0xRRGGBB), channel by channel: paper takes the tint, ink stays black.
void multiply(unsigned char* data, int stride, Rect r, uint32_t tint) {
    const uint32_t tr = (tint >> 16) & 0xff, tg = (tint >> 8) & 0xff, tb = tint & 0xff;
    for (int y = r.y; y < r.y + r.h; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
        for (int x = r.x; x < r.x + r.w; ++x) {
            const uint32_t p = row[x];
            row[x] = 0xff000000u | (((p >> 16) & 0xff) * tr / 255) << 16 | (((p >> 8) & 0xff) * tg / 255) << 8 | (p & 0xff) * tb / 255;
        }
    }
}

// An area's tint on a color screen: its colour 15% over white, a third of a highlight's 45% (as in gray).
uint32_t areaTint(const std::string& colour) {
    const uint32_t c = zoteroRgb(colour);
    uint32_t out = 0;
    for (int shift : {16, 8, 0}) out |= static_cast<uint32_t>(0.15 * ((c >> shift) & 0xff) + 0.85 * 255 + 0.5) << shift;
    return out;
}

// Zotero highlights (read-only): the page under the box is multiplied by the shade, so paper
// darkens and ink stays black; underlines are a bar under the text. `color` (a color screen):
// multiplied by the colour's tint instead (highlightTint), and underlines, areas' frames, notes'
// icons and ink in the colour itself.
void drawHighlights(Canvas& c, const std::vector<HighlightBox>& boxes, bool color) {
    if (boxes.empty()) return;
    cairo_surface_flush(c.surface());
    unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (const auto& b : boxes) {
        const Rect r = b.rect.intersected(ReaderScreen::pageArea()).intersected(c.paintRegion());
        if (!r.empty() && b.kind == HighlightBox::Kind::Image && color) {
            multiply(data, stride, r, areaTint(b.color));
            continue;
        }
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
            const uint32_t bar = color ? 0xff000000u | zoteroRgb(b.color) : 0xff3c3c3cu;
            // At the bottom of the box itself (r may be cut by the paint region), inside r's rows
            // and the region's.
            const Rect box = b.rect.intersected(ReaderScreen::pageArea());
            const Rect region = c.paintRegion();
            const int bottom = box.y + box.h;
            for (int y = std::max({r.y, bottom - 5, region.y}); y < bottom + 1 && y < region.y + region.h && y < c.height(); ++y) {
                auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
                for (int x = r.x; x < r.x + r.w; ++x) row[x] = bar;
            }
            continue;
        }
        if (color) {
            multiply(data, stride, r, highlightTint(b.color));
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
            case HighlightBox::Kind::Note: {  // the panel's Add note icon, the size of Zotero's: its colour, an ink outline
                const int s = noteIcon(b.rect).w;
                const double l = b.rect.x + 1.5, t = b.rect.y + 1.5, r = b.rect.x + s - 1.5, btm = b.rect.y + s - 1.5;
                const double fold = s * 0.37;
                cairo_save(cr);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_move_to(cr, l, t);
                cairo_line_to(cr, r, t);
                cairo_line_to(cr, r, btm - fold);
                cairo_line_to(cr, r - fold, btm);
                cairo_line_to(cr, l, btm);
                cairo_close_path(cr);
                if (color) rgb(cr, zoteroRgb(b.color));
                else gray(cr, b.shade);
                cairo_fill_preserve(cr);
                gray(cr, kInk);
                cairo_set_line_width(cr, 3);
                cairo_stroke(cr);
                cairo_move_to(cr, r, btm - fold);  // the folded corner
                cairo_line_to(cr, r - fold, btm - fold);
                cairo_line_to(cr, r - fold, btm);
                cairo_stroke(cr);
                cairo_restore(cr);
                break;
            }
            case HighlightBox::Kind::Image:  // the area: a 3 px frame, its colour as a darker shade (or itself)
                if (color) rgb(cr, zoteroRgb(b.color));
                else gray(cr, areaFrameShade(b.shade));
                cairo_set_line_width(cr, 3);
                cairo_rectangle(cr, b.rect.x + 1.5, b.rect.y + 1.5, b.rect.w - 3, b.rect.h - 3);
                cairo_stroke(cr);
                break;
            case HighlightBox::Kind::Ink:
                if (color && !b.color.empty()) rgb(cr, zoteroRgb(b.color));
                else gray(cr, kInk);
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

// Ink in its colour on a color screen (`colour`: 0xRRGGBB), else black.
void strokePaths(cairo_t* cr, const std::vector<std::vector<Point>>& paths, int widthPx,
                 std::optional<uint32_t> colour = std::nullopt) {
    if (paths.empty()) return;
    if (colour) rgb(cr, *colour);
    else gray(cr, kInk);
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
        const Rect r = box.intersected(ReaderScreen::pageArea()).intersected(c.paintRegion());
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
constexpr const char* kPenButtonKey = "penButton";  // "highlight": the side button highlights
constexpr const char* kInkColourKey = "inkColour";  // the pen's ink, a Zotero colour or "#000000"
constexpr const char* kPenOnKindleKey = "penUsed";  // "1": a pen has been used on this Kindle
constexpr int64_t kPalmTurnWaitMs = 300;  // a page-turn tap waits this long for the pen (a palm's)
constexpr int64_t kAfterPenMs = 1500;     // no finger page turns this soon after the pen
constexpr const char* kPenWidthKey = "penWidth";  // "0" thin, "1" medium (default), "2" thick

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
    : row_(std::move(row)), path_(std::move(path)), type_(type), web_(row_.badge == "WEB"), makeWorker_(std::move(makeWorker)),
      flashEvery_(flashEvery > 0 ? flashEvery : 6), lipc_(lipc) {}

ReaderScreen::~ReaderScreen() = default;

Rect ReaderScreen::pageArea() { return {0, 0, W(), H() - kFooterH}; }
Rect ReaderScreen::footerArea() { return {0, H() - kFooterH, W(), kFooterH}; }

bool ReaderScreen::wantsKeyboard() const { return note_.has_value() || (editor_ && editor_->wantsKeyboard()); }

void ReaderScreen::readLibrary(Nav& nav) {
    if (preview_) return;  // not the library's: read-only, as setPreview made it
    const auto att = nav.library().attachment(row_.attachmentKey);
    const int64_t lib = att ? att->lib : row_.lib;
    readOnly_ = lib != kMyLibrary && !nav.library().libraryEditable(lib);
}

bool ReaderScreen::refuseReadOnly(Nav& nav, bool always) {
    if (!readOnly_) return false;
    if (always || !readOnlyTold_) nav.toast(preview_ ? "A preview can't be marked: keep it in your library first." : std::string(kReadOnlyGroup) + ".");
    readOnlyTold_ = true;
    return true;
}

void ReaderScreen::onShow(Nav& nav) {
    notePicking_ = false;
    readLibrary(nav);
    if (worker_) {  // back from a pushed panel: the book stays open (its notes may have changed)
        sendAnnotations(nav, true);  // unchanged: the app's redraw is the only repaint
        return;
    }
    twoColumnsSetting_ = loadSettings(nav.library(), nav.device()).twoColumns;
    layout_ = loadLayout(nav.library(), row_.key, type_, twoColumnsSetting_);
    if (const std::string c = nav.device().get(kColourKey).value_or(""); !c.empty()) colour_ = c;
    penWrites_ = nav.device().get(kPenModeKey).value_or("") != "tap";
    penButtonHighlights_ = nav.device().get(kPenButtonKey).value_or("") == "highlight";
    if (const std::string c = nav.device().get(kInkColourKey).value_or(""); !c.empty()) inkColour_ = c;
    penOnKindle_ = nav.device().get(kPenOnKindleKey).value_or("") == "1";
    if (const std::string w = nav.device().get(kPenWidthKey).value_or(""); w == "0" || w == "2") penWidth_ = w[0] - '0';
    nav.setSystemInkWidth(inkWidthPx());
    DocPosition start;
    start.value = preview_ ? preview_->position : nav.library().readingLocation(row_.attachmentKey);
    // Zotero's reading position: where to open a book never read here, or a question if the
    // computer moved on since this Kindle last agreed with it.
    // Held (nothing sent) until it has been compared with the page the book opens on.
    std::string zoteroStart;
    // A web snapshot's isn't used: Zotero keeps it as a scroll percentage, which doesn't map onto pages.
    if (auto rp = web_ || preview_ ? std::nullopt : nav.library().readingPosition(row_.attachmentKey);
        rp && !rp->remote.empty() && (start.value.empty() || rp->local.empty() || rp->remoteNew)) {
        nav.library().holdRemotePosition(row_.attachmentKey);
        if (start.value.empty()) zoteroStart = rp->remote;
        placeAfterFirstPage_ = remoteValue_ = rp->remote;
        remoteVersion_ = rp->remoteVersion;
    }
    worker_ = makeWorker_();
    colorOn_ = nav.colorCaps().on;
    worker_->open(path_, type_, layout_, start, zoteroStart, colorOn_, colorBoostPct(std::getenv("ZK_COLOR_BOOST")));
    if (!preview_) marks_ = nav.library().bookmarks(row_.attachmentKey);
    sendBookmarks();
    if (openAt_) {
        turning_ = true;
        turnTarget_ = -1;
        worker_->showAnnotation(*openAt_);
        openAt_.reset();
    }
    sendAnnotations(nav, false);
}

void ReaderScreen::onTick(Nav& nav) {
    if (palmTap_ && nav.nowMs() >= palmTap_->dueMs) {  // the pen didn't come: a finger's tap
        runPalmTap(nav);
    } else if (palmTap_) {
        nav.wakeAt(palmTap_->dueMs);
    }
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
    if (const bool undo = penControls() && canUndoInk(); undo != undoShown_) {  // Undo comes or goes
        undoShown_ = undo;
        if (!menus_) nav.damage(footerArea(), Waveform::Ui);
    }
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
    selTouched_ = {};
    selAutoHighlight_ = false;
    bar_.reset();
    selTags_.clear();
    note_.reset();
    notePlace_.reset();
}

void ReaderScreen::pumpEvents(Nav& nav, bool visible) {
    if (!worker_) return;
    if (nav.colorCaps().on != colorOn_) {  // Display → Color: the pages again, in color or gray (the app redraws the rest)
        colorOn_ = !colorOn_;
        worker_->setColor(colorOn_, colorBoostPct(std::getenv("ZK_COLOR_BOOST")));
    }
    RenderEvent e;
    while (worker_->poll(e)) {
        switch (e.kind) {
            case RenderEvent::Kind::Opened:
                pageCount_ = e.pageCount;
                views_ = std::max(1, e.viewsPerPage);
                toc_ = e.toc;
                if (!openedSent_ && !preview_) {  // the book really opened (not again after a re-layout)
                    openedSent_ = true;
                    plugins::bookEvent(nav.plugins(), nav.library(), "bookOpened", row_.key, row_.attachmentKey);
                }
                break;
            case RenderEvent::Kind::Error:
                error_ = e.error;
                bookmarkArriving_ = false;  // no page arrives
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
                const bool again = std::exchange(placingAgain_, false);  // after a re-layout: nothing to compare
                if (e.page < 0) {  // can't be placed here: nothing is sent until the reader reads on
                    if (!again) {
                        unplaced_ = true;
                    } else if (question_) {  // not in the new pages: no Go, the banner's place is gone with it
                        question_.reset();
                        unplaced_ = true;
                        if (visible && !menus_) nav.damage(positionBannerRect(), Waveform::Quality);
                    }
                    break;
                }
                remotePage_ = e.page / std::max(1, e.viewsPerPage);  // a PDF page, whatever the View
                const int pct = static_cast<int>(std::lround(e.progress * 100));
                const std::string text = (type_ == DocType::Pdf ? "Your computer is on page " + std::to_string(shownPage(e.page)) + "."
                                                                : "Your computer is at " + std::to_string(pct) + "%" +
                                                                      (e.chapter.empty() ? "" : " (" + e.chapter + ")") + ".") +
                                         "\nGo there?";
                if (again) {
                    if (question_ && question_->text != text) {  // the new pages' percentage and chapter
                        question_->text = text;
                        if (visible && !menus_) nav.damage(positionBannerRect(), Waveform::Ui);
                    }
                    // The re-layout's page named its start: on Zotero's page, Zotero's exact place it is.
                    if (remotePage_ == page_ / views_ && !web_ && !preview_)
                        nav.library().setLocalPosition(row_.attachmentKey, remoteValue_);
                    break;
                }
                if (remotePage_ == page_ / views_) {  // the page we're on: agreed, nothing to ask
                    nav.library().answerRemotePosition(row_.attachmentKey, true, remoteVersion_);
                    break;
                }
                question_ = PositionQuestion{text};
                if (visible) nav.damage(positionBannerRect(), Waveform::Ui);
                break;
            }
            case RenderEvent::Kind::Ink:
                onInk(e, nav);
                break;
            case RenderEvent::Kind::Area:
                onArea(e, nav);
                break;
            case RenderEvent::Kind::Words:  // the time left, counted after the page was shown
                if (e.page != page_ || turning_ || e.wordsLeftInChapter == wordsLeft_) break;
                {
                    const std::string before = footerLeft();
                    wordsLeft_ = e.wordsLeftInChapter;
                    if (visible && !menus_ && footerLeft() != before) nav.damage(footerArea(), Waveform::Ui);
                }
                break;
            case RenderEvent::Kind::Page: {
                const bool first = image_ == nullptr;
                const bool chapterChanged = !first && e.chapter != chapter_;
                const bool turned = turning_;
                // The re-layout's page (or a later one): one posted before it keeps the old numbering.
                const bool relaid = layoutPending_ && e.layoutId == layoutId_;
                if (e.page != page_ && (selecting_ || selection_ || editor_ || areaDrawing_ || note_)) {  // the page changed under them
                    if (note_ || (editor_ && editor_->wantsKeyboard())) nav.keyboard().hide();
                    editor_.reset();
                    dropSelection();
                }
                if (e.page != page_) dropArea();
                // The same page again (new highlights, a bookmark, the time left): only what changed
                // is repainted, not the whole page twice over (a saved highlight was drawn already).
                const bool samePage = !first && !turned && !relaid && e.page == page_ && e.image == image_ &&
                                      e.pageBox == pageBox_ && !chapterChanged;
                const std::vector<HighlightBox> highlightsBefore = samePage ? highlights_ : std::vector<HighlightBox>{};
                const std::string footerBefore = backLabel().value_or("") + '\x1f' + footerLeft() + '\x1f' + footerRight();
                const bool marksMoved = markPlaces_ != e.bookmarkPlaces;
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
                if (turned && turnTarget_ >= 0 && e.page == turnTarget_)
                    log(LogLevel::Debug, "turn to page %d: posted after %lld ms", e.page,
                        static_cast<long long>(nav.nowMs() - turnAskedMs_));
                if (turnTarget_ < 0 || e.page == turnTarget_) {  // else posted before a later turn overtook it
                    turning_ = false;
                    turnTarget_ = -1;
                }
                if (relaid) {
                    layoutPending_ = false;
                    inkUndo_.clear();  // its page numbers are views of the old layout
                }
                if (preview_) {
                    if (preview_->savePosition) preview_->savePosition(position_.value);
                } else {
                    nav.library().saveReading(row_.key, row_.attachmentKey, position_.value, progress_);
                }
                markPlaces_ = e.bookmarkPlaces;
                links_ = e.links;
                if (turned) linkPopup_.reset();  // a pop-up belongs to the page it opened on
                if (e.landed && arriving_) {  // a jump landed; one onto the page it left needs no Back
                    arriving_ = false;
                    if (!back_.empty() && back_.back().page == page_) back_.pop_back();
                }
                // On Zotero's page, Zotero's exact place (not this page's start) stays the Kindle's.
                if (!e.zoteroPosition.empty() && !web_ && !preview_)
                    nav.library().setLocalPosition(row_.attachmentKey,
                                                   page_ / views_ == remotePage_ ? remoteValue_ : e.zoteroPosition);
                if (!placeAfterFirstPage_.empty()) {
                    placeId_ = worker_->placeZotero(std::move(placeAfterFirstPage_));
                    placeAfterFirstPage_.clear();
                }
                if (relaid && pendingGo_) {  // Go to page or the slider, asked while the pages were counted again
                    const PendingGo go = *pendingGo_;
                    pendingGo_.reset();
                    // A PDF page stays that page (in its views now); a book's page, the same place in it.
                    const int to = type_ == DocType::Pdf ? go.page / go.views * views_
                                   : go.count > 1     ? static_cast<int>(std::lround(static_cast<double>(go.page) *
                                                                                      std::max(0, pageCount_ - 1) / (go.count - 1)))
                                                      : 0;
                    turnTo(std::clamp(to, 0, std::max(0, pageCount_ - 1)));
                }
                if (bookmarkArriving_ && !turning_ && !layoutPending_ && !position_.value.empty()) {
                    bookmarkArriving_ = false;
                    if (!bookmarked()) toggleBookmark(nav);  // a tap made blind adds, never removes
                }
                if (!visible) break;
                Waveform w = Waveform::Quality;
                // A new chapter flashes in a book; a PDF's sections come too often for that.
                const bool chapterFlash = chapterChanged && type_ != DocType::Pdf;
                if (turned && (++turns_ % flashEvery_ == 0 || chapterFlash)) w = Waveform::Full;
                if (!turned && chapterChanged) w = Waveform::Full;
                if (menus_) {
                    menus_->onPage(nav);
                } else if (samePage) {
                    for (const Rect& r : changedBoxes(highlightsBefore, highlights_)) nav.damage(r.intersected(pageArea()), Waveform::Quality);
                    if (marksMoved) nav.damage(bookmarkRect(), Waveform::Quality);
                    if (backLabel().value_or("") + '\x1f' + footerLeft() + '\x1f' + footerRight() != footerBefore)
                        nav.damage(footerArea(), Waveform::Quality);
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
    // A turn while one is on its way goes on from the page it is going to.
    const int target = (turning_ && turnTarget_ >= 0 ? turnTarget_ : page_) + delta;
    if (delta > 0 && pageCount_ > 0 && target >= pageCount_ && !finishedSent_ && !preview_) {
        finishedSent_ = true;  // once a reading, however often the last page is swiped on
        plugins::bookEvent(nav.plugins(), nav.library(), "bookFinished", row_.key, row_.attachmentKey);
    }
    if (delta > 0 && pageCount_ > 0 && target >= pageCount_ && !endPrompt_ && !preview_ && !nav.library().isRead(row_.key)) {
        endPrompt_ = true;
        nav.damage(endPromptRect(), Waveform::Ui);
    }
    if (target < 0 || (pageCount_ > 0 && target >= pageCount_)) return;
    if (!back_.empty() && ++turnsSinceJump_ >= kBackTurns) back_.clear();  // read on: no way back
    // Reading on answers Zotero's position: the Kindle's place it is.
    // (Also while Zotero's position is still being placed, or can't be.)
    if (question_) {
        answerPosition(false, nav);
    } else if (!placingAgain_ && (unplaced_ || placeId_ || !placeAfterFirstPage_.empty())) {
        unplaced_ = false;
        placeId_ = 0;
        placeAfterFirstPage_.clear();
        nav.library().answerRemotePosition(row_.attachmentKey, false, remoteVersion_);
    }
    turning_ = true;
    turnTarget_ = target;
    turnAskedMs_ = nav.nowMs();
    worker_->show(target);
}

void ReaderScreen::turnTo(int page) {
    dropArea();  // a jump or a turn ends Mark an area and a half-drawn area
    if (!worker_) return;
    if (layoutPending_) {  // counted in pages about to change: gone to once the new ones are counted
        pendingGo_ = PendingGo{page, pageCount_, views_};
        return;
    }
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
    turnTarget_ = -1;  // setLayout drops a turn on its way: the re-layout's page ends it
    placeRemoteAgain();
    layoutId_ = worker_->setLayout(layout_, position_);  // keeps the passage; works before the first page too
}

void ReaderScreen::placeRemoteAgain() {
    if (remoteValue_.empty() || !placeAfterFirstPage_.empty()) return;  // asked once the new pages show anyway
    if (placeId_) {  // its answer counts the old pages: ask again (as what it was, first or again)
        placeId_ = 0;
        placeAfterFirstPage_ = remoteValue_;
        return;
    }
    if (remotePage_ < 0) return;
    remotePage_ = -1;
    placingAgain_ = true;
    placeAfterFirstPage_ = remoteValue_;
}

void ReaderScreen::setTwoColumns(bool on, Nav& nav) {
    AppSettings s = loadSettings(nav.library(), nav.device());
    s.twoColumns = twoColumnsSetting_ = on ? 1 : 0;
    saveSettings(nav.library(), nav.device(), s);
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
    turnTarget_ = -1;  // setLayout drops a turn on its way: the re-layout's page ends it
    for (auto& b : back_) b.page = -1;  // their pages are counted again: Back keeps the place, not the number
    placeRemoteAgain();
    if (!preview_) saveLayout(nav.library(), row_.key, l);  // a preview's lasts while it is open
    layoutId_ = worker_->setLayout(l, position_);
}

void ReaderScreen::openMenus(Nav& nav) {
    linkId_ = 0;
    paletteOpen_ = false;
    palmTap_.reset();
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

void ReaderScreen::savePreviewAndClose(Nav& nav) {
    if (!preview_) return;
    previewCloseAction_ = "save";
    nav.pop();
}

void ReaderScreen::requestPreviewClose(Nav& nav) {
    if (!preview_) {
        nav.pop();
        return;
    }
    const bool ask = preview_->saveMode == PreviewSaveMode::Prompt ||
                     (preview_->saveMode == PreviewSaveMode::Allow && !previewAnnotations_.empty());
    if (!ask) {
        nav.pop();
        return;
    }
    if (menus_) closeMenus(nav);
    const size_t n = previewAnnotations_.size();
    std::vector<std::string> lines;
    const std::string title = row_.title.empty() ? "this item" : "“" + row_.title + "”";
    lines.push_back("Save " + title + " to Zotero?");
    if (n > 0) {
        lines.push_back(std::to_string(n) + (n == 1 ? " highlight will be attached as a note." : " highlights will be attached as a note."));
    }
    const std::string saveLabel = n > 0 ? "Save (" + std::to_string(n) + ")" : "Save";
    previewClosePrompt_ = std::make_unique<PluginConfirm>("Preview", std::move(lines), saveLabel, "Discard");
    nav.damage(previewClosePrompt_->rect(), Waveform::Ui);
}

void ReaderScreen::onGesture(const Gesture& g, Nav& nav) {
    if (error_) {
        if (g.type == GestureType::Tap) nav.pop();  // "Back"
        return;
    }
    if (previewClosePrompt_) {
        if (g.type != GestureType::Tap) return;
        const Rect r = previewClosePrompt_->rect();
        if (const auto ans = previewClosePrompt_->onTap(g.pos)) {
            previewClosePrompt_.reset();
            previewCloseAction_ = *ans ? "save" : "discard";
            if (!*ans) previewAnnotations_.clear();
            nav.pop();
            return;
        }
        if (!r.contains(g.pos)) {  // tap outside: keep reading
            previewClosePrompt_.reset();
            nav.damage(r, Waveform::Quality);
        }
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
    if (paletteOpen_) {  // any gesture closes it; a tap on a colour chooses it
        paletteOpen_ = false;
        if (g.type == GestureType::Tap)
            for (int row = 0; row < 2; ++row)
                for (int i = 0; i < (row ? 9 : 8); ++i)
                    if (paletteDot(row, i).contains(g.pos)) choosePaletteColour(row, i, nav);
        nav.damage(paletteRect(), Waveform::Quality);
        nav.damage(footerArea(), Waveform::Ui);
        return;
    }
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
    const bool guarded = palmGuarded(g);
    const bool justWrote = guarded && lastPenMs_ >= 0 && nav.nowMs() - lastPenMs_ < kAfterPenMs;
    if (g.type == GestureType::Tap && !editor_ && !note_ && !selecting_ && !selection_ && ready()) {
        const bool onBack = backLabel() && backRect().contains(g.pos);
        const bool onBookmark = !preview_ && bookmarkRect().contains(g.pos) && !(question_ && positionBannerRect().contains(g.pos));
        const bool onLink = !onBack && !onBookmark && !(question_ && positionBannerRect().contains(g.pos)) && linkNear(g.pos);
        if (onBack || onBookmark || onLink) {
            if (guarded) {
                if (justWrote) return;
                PalmTap pt;
                pt.back = onBack;
                pt.bookmark = onBookmark;
                if (onLink) pt.link = g.pos;
                return holdForPalm(nav, pt);
            }
            if (onBack) return back(nav);
            if (onBookmark) {
                // Mid-turn or re-laying out, the page under the corner is going: the one that arrives is marked.
                if (turning_ || layoutPending_) bookmarkArriving_ = true;
                else if (!position_.value.empty()) toggleBookmark(nav);
                return;
            }
            if (tapLink(g.pos, nav)) return;
        }
    }
    if (question_ && g.type == GestureType::Tap) {  // reading on (a page turn) is Stay: see turn()
        if (positionGoRect().contains(g.pos)) return answerPosition(true, nav);
        if (positionStayRect().contains(g.pos)) return answerPosition(false, nav);
        if (positionBannerRect().contains(g.pos)) return;
    }
    if (editor_) {
        if (editor_->commenting() && g.type != GestureType::Tap) return;  // typing: only a tap ends it
        if (g.type == GestureType::Tap && editor_->onTap(g.pos, nav) == AnnotationEditor::Result::Open) {
            if (editor_->takeChanged()) {
                refreshHighlights(nav);
                if (!previewCanSave()) nav.sync().pushSoon();
                if (editor_->key() == newAreaKey_ && editor_->bar().colour() != colour_) {  // as a new highlight's
                    colour_ = editor_->bar().colour();
                    nav.device().set(kColourKey, colour_);
                }
            }
            return;
        }
        closeEditor(nav);
        if (g.type == GestureType::Tap) return;
    }
    if (note_) {  // typing a note: a tap elsewhere keeps what was typed (nothing typed: gives up)
        if (g.type == GestureType::Tap && !noteRect().contains(g.pos)) {
            const std::string text = note_->text();
            if (notePlace_) return saveNote(text, nav);
            if (text.find_first_not_of(" \t\n") == std::string::npos) return closeSelection(nav);
            return saveSelection("highlight", text, nav);
        }
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
        // The hand resting between strokes. One resting before the pen came selects, and the pen
        // coming drops it (onPen).
        if (justWrote) return;
        if (ready() && pageArea().contains(g.pos)) {
            startSelection(g.pos, nav);
            fingerSelecting_ = guarded;
        }
        return;
    }
    if (g.type == GestureType::Swipe) {
        if (justWrote) return;  // the hand moving between strokes
        const int delta = g.dir == Direction::Left ? +1 : g.dir == Direction::Right ? -1 : 0;
        if (delta != 0 && guarded) {
            PalmTap pt;
            pt.delta = delta;
            return holdForPalm(nav, pt);  // a palm sliding to rest
        }
        if (delta != 0) turn(delta, nav);
        return;
    }
    if (g.type != GestureType::Tap) return;
    const bool onChip = penControls() && ready() && colourChipRect().contains(g.pos);
    const bool onUndo = penControls() && ready() && undoShown_ && undoRect().contains(g.pos);
    if (onChip || onUndo) {
        // A palm settles where these are too (bottom right): a finger's tap waits for the pen like a
        // turn's, even right after writing (that's when Undo is wanted).
        if (guarded) {
            PalmTap pt;
            pt.undos = onUndo ? 1 : 0;
            pt.chip = onChip;
            return holdForPalm(nav, pt);
        }
        if (onUndo) return undoInk(nav);
        paletteOpen_ = true;
        nav.damage(paletteRect(), Waveform::Ui);
        return;
    }
    // A highlight anywhere opens its editor (Pavel); otherwise the outer 30% turns, the middle opens
    // the menus. Where the palm may be, each waits for the pen (onTick), as a page turn does: a
    // hand settling to write must not open a highlight or the menus, which take the pen too.
    const int delta = g.pos.x > W() * 7 / 10 ? +1 : g.pos.x < W() * 3 / 10 ? -1 : 0;
    if (guarded) {
        if (justWrote) return;
        if (ready() && annotationNear(g.pos)) {
            PalmTap pt;
            pt.edit = g.pos;
            return holdForPalm(nav, pt);
        }
        if (delta == 0 && !ready()) return;
        PalmTap pt;
        pt.delta = delta;
        pt.menus = delta == 0;
        return holdForPalm(nav, pt);
    }
    if (ready() && editAt(g.pos, nav)) return;
    if (delta != 0) turn(delta, nav);
    else if (ready()) openMenus(nav);  // not before the first page
}

void ReaderScreen::holdForPalm(Nav& nav, const PalmTap& add) {
    if (!palmTap_) palmTap_ = PalmTap{};
    palmTap_->delta += add.delta;
    palmTap_->undos += add.undos;
    palmTap_->chip |= add.chip;
    palmTap_->back |= add.back;
    palmTap_->bookmark |= add.bookmark;
    palmTap_->menus |= add.menus;
    if (add.link) palmTap_->link = add.link;
    if (add.edit) palmTap_->edit = add.edit;
    palmTap_->dueMs = nav.nowMs() + kPalmTurnWaitMs;
    nav.wakeAt(palmTap_->dueMs);
}

// The held taps, now that the pen didn't come; dropped if something opened meanwhile (menus, an
// editor, a pop-up), which the taps would act behind.
void ReaderScreen::runPalmTap(Nav& nav) {
    const PalmTap t = *palmTap_;
    palmTap_.reset();
    if (menus_ || editor_ || selection_ || selecting_ || linkPopup_ || notePopup_ || note_ || endPrompt_ || paletteOpen_ ||
        areaArmed_ || noteArmed_ || !ready())
        return;
    for (int i = 0; i < t.undos; ++i) undoInk(nav);
    if (t.chip) {
        paletteOpen_ = true;
        nav.damage(paletteRect(), Waveform::Ui);
        return;
    }
    if (t.back && backLabel()) return back(nav);
    if (t.bookmark && !preview_) {
        if (turning_ || layoutPending_) bookmarkArriving_ = true;
        else if (!position_.value.empty()) toggleBookmark(nav);
        return;
    }
    if (t.link && tapLink(*t.link, nav)) return;
    if (t.edit && editAt(*t.edit, nav)) return;
    if (t.menus) return openMenus(nav);
    // Quick taps add up, but not past the last page (one more there: the end of the book) or the first.
    const int from = turning_ && turnTarget_ >= 0 ? turnTarget_ : page_;
    int delta = t.delta;
    if (delta > 0) delta = std::min(delta, std::max(1, pageCount_ - 1 - from));
    if (delta < 0) delta = std::max(delta, -std::max(1, from));
    if (delta != 0) turn(delta, nav);
}

// --- Pen controls: the colour chip, its palette, Undo -------------------------------------------

bool ReaderScreen::penControls() const {
    return type_ == DocType::Pdf && penOnKindle_ && !readOnly_ && !web_ && !preview_ && image_ && !error_;
}
Rect ReaderScreen::colourChipRect() { return {W() - kMargin - 260 - 100, H() - kFooterH, 90, kFooterH}; }
Rect ReaderScreen::undoRect() { return {colourChipRect().x - 150, H() - kFooterH, 140, kFooterH}; }
Rect ReaderScreen::paletteRect() {
    const int w = std::min(W() - 2 * kMargin, 1090);
    return {W() - kMargin - w, H() - kFooterH - 250, w, 240};
}
Rect ReaderScreen::paletteDot(int row, int i) {
    const Rect p = paletteRect();
    return {p.x + 210 + i * 96, p.y + 20 + row * 110, 96, 100};
}

std::optional<uint32_t> ReaderScreen::inkRgb(const std::string& colour) const {
    if (!colorOn_ || colour == "#000000") return std::nullopt;
    return zoteroRgb(colour);
}

void ReaderScreen::choosePaletteColour(int row, int i, Nav& nav) {
    if (row == 0) {
        colour_ = zoteroColours()[i].hex;
        nav.device().set(kColourKey, colour_);
        return;
    }
    const std::string hex = i == 0 ? "#000000" : zoteroColours()[i - 1].hex;
    if (hex == inkColour_) return;
    commitInk();  // the strokes so far keep their colour: one annotation, one colour
    inkColour_ = hex;
    nav.device().set(kInkColourKey, inkColour_);
}

bool ReaderScreen::canUndoInk() const {
    if (inkPage_ == page_ && !inkPending_.empty()) return true;
    for (const auto& [id, s] : inkSent_)
        if (s.page == page_ && !s.paths.empty()) return true;
    for (const auto& u : inkUndo_)
        if (u.page == page_) return true;
    return false;
}

// The last stroke written on this page goes: not saved yet, being placed, or from the ink
// annotation it became (rewritten without it, deleted when it was the last, as the eraser does).
void ReaderScreen::undoInk(Nav& nav) {
    Rect gone;
    int pad = inkWidthPx();
    auto take = [&](std::vector<std::vector<Point>>& paths) {
        for (const Point& q : paths.back()) gone = gone.united({q.x, q.y, 1, 1});
        paths.pop_back();
    };
    // The newest group on this page: being written, being placed, or saved (by when it was written).
    SentInk* sent = nullptr;
    for (auto& [id, s] : inkSent_)
        if (s.page == page_ && !s.paths.empty() && (!sent || s.seq > sent->seq)) sent = &s;
    uint64_t savedSeq = 0;
    for (const auto& u : inkUndo_)
        if (u.page == page_) savedSeq = std::max(savedSeq, u.seq);
    if (inkPage_ == page_ && !inkPending_.empty()) {
        take(inkPending_);
    } else if (sent && sent->seq >= savedSeq) {
        take(sent->paths);
        sent->erased = true;  // placed again without it (or not saved at all, if none is left)
    } else {
        while (!inkUndo_.empty()) {
            auto it = std::max_element(inkUndo_.begin(), inkUndo_.end(), [&](const UndoInk& a, const UndoInk& b) {
                return (a.page == page_ ? a.seq + 1 : 0) < (b.page == page_ ? b.seq + 1 : 0);
            });
            if (it == inkUndo_.end() || it->page != page_) break;
            const UndoInk entry = *it;
            const std::string key = entry.key;
            inkUndo_.erase(it);
            auto rec = nav.library().annotation(key);
            if (!rec || rec->deletedHere || !nav.library().annotationEditable(*rec)) continue;  // gone meanwhile
            nlohmann::json pos = nlohmann::json::parse(rec->position, nullptr, false);
            if (pos.is_discarded() || !pos.contains("paths") || pos["paths"].empty()) continue;
            const size_t strokes = pos["paths"].size();
            if (strokes == 1 && (!rec->comment.empty() || !rec->tags.empty())) {
                inkUndo_.push_back(entry);  // stays undoable once they're gone
                nav.toast("This handwriting has a comment or tags: delete it in Notes.");
                return;
            }
            if (strokes > 1) {
                pos["paths"].erase(strokes - 1);
                rec->position = pos.dump();
                if (!nav.library().editAnnotation(*rec)) return;
                inkUndo_.push_back(entry);  // its earlier strokes can go next
                plugins::annotationEvent(nav.plugins(), nav.library(), "highlightChanged", key);
            } else {
                const bool told = nav.plugins().listening();
                const plugins::AnnotationRef ref = told ? plugins::annotationRef(nav.library(), *rec) : plugins::AnnotationRef{};
                if (!nav.library().deleteAnnotation(key)) return;
                if (told) plugins::annotationDeleted(nav.plugins(), ref);
            }
            // Shown at once: the stroke off the page's drawing and off the strokes kept since saving.
            for (auto& b : highlights_)
                if (b.annotationKey == key && b.paths.size() == strokes) {
                    pad = std::max(pad, b.strokePx);
                    take(b.paths);
                }
            for (auto& s : inkSaved_)
                if (s.key == key && s.paths.size() == strokes) take(s.paths);
            highlights_.erase(std::remove_if(highlights_.begin(), highlights_.end(),
                                             [](const HighlightBox& b) { return b.kind == HighlightBox::Kind::Ink && b.paths.empty(); }),
                              highlights_.end());
            nav.sync().pushSoon();
            refreshHighlights(nav);
            break;
        }
    }
    if (!gone.empty()) nav.damage(Rect{gone.x - pad - 4, gone.y - pad - 4, gone.w + 2 * pad + 8, gone.h + 2 * pad + 8}.intersected(pageArea()), Waveform::Quality);
}

// A finger's touch on a PDF page taking handwriting, on a Kindle with a pen: maybe the palm.
bool ReaderScreen::palmGuarded(const Gesture& g) const {
    return type_ == DocType::Pdf && penWrites_ && penOnKindle_ && !readOnly_ && g.finger >= 0 && g.finger != kPenFinger;
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

// On a box first; else the nearest box within a fingertip (thin PDF lines are hard to hit).
// An area is hit near its frame (inside or out), not in its middle, which stays the page's
// (menus, turns, links, highlights under it); a small area anywhere inside. Nearest wins.
// Notes are noteNear's. Empty: none.
std::string ReaderScreen::highlightKeyAt(Point p) const {
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
    return key;
}

// An annotation a tap here would open (a note's pop-up, a highlight's editor).
bool ReaderScreen::annotationNear(Point p) const { return noteNear(p) || !highlightKeyAt(p).empty(); }

bool ReaderScreen::editAt(Point p, Nav& nav) {
    if (openNoteAt(p, nav)) return true;
    const std::string key = highlightKeyAt(p);
    if (key.empty()) return false;
    if (previewCanSave()) {
        auto it = std::find_if(previewAnnotations_.begin(), previewAnnotations_.end(),
                               [&](const AnnotationRecord& r) { return r.key == key; });
        if (it == previewAnnotations_.end()) return false;
        Rect anchor;
        for (const auto& h : highlights_)
            if (h.annotationKey == key) anchor = anchor.united(h.rect);
        linkId_ = 0;
        AnnotationEditor::Store store;
        store.edit = [this](const AnnotationRecord& updated) {
            for (auto& r : previewAnnotations_)
                if (r.key == updated.key) r = updated;
        };
        store.remove = [this](const std::string& k) {
            previewAnnotations_.erase(
                std::remove_if(previewAnnotations_.begin(), previewAnnotations_.end(),
                               [&](const AnnotationRecord& r) { return r.key == k; }),
                previewAnnotations_.end());
        };
        editor_ = std::make_unique<AnnotationEditor>(*it, anchor, recentChips(nav.library()), "", std::move(store));
        nav.damage(editor_->rect(), Waveform::Ui);
        return true;
    }
    auto a = nav.library().annotation(key);
    if (!a) return false;
    Rect anchor;
    for (const auto& h : highlights_)
        if (h.annotationKey == key) anchor = anchor.united(h.rect);
    linkId_ = 0;
    editor_ = std::make_unique<AnnotationEditor>(*a, anchor, recentChips(nav.library()), annotationReadOnly(nav.library(), *a));
    nav.damage(editor_->rect(), Waveform::Ui);
    if (a->type == "image")  // its frame turns to ink while selected
        for (const Rect& r : frameEdges(anchor, 3)) nav.damage(r.intersected(pageArea()), Waveform::Ui);
    return true;
}

void ReaderScreen::closeEditor(Nav& nav) {
    if (!editor_) return;
    if (editor_->takeChanged()) {
        refreshHighlights(nav);
        if (!previewCanSave()) nav.sync().pushSoon();
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
    std::vector<AnnotationRecord> anns = previewCanSave() ? previewAnnotations_ : pageAnnotations(nav.library(), row_.attachmentKey);
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
    selAutoHighlight_ = false;  // a side-button stroke sets it again after this
    selAnchor_ = selLast_ = at;
    requestSelection(at);
    selShown_ = selPending_ - 1;  // answers to an earlier selection's requests are older still
}

void ReaderScreen::requestSelection(Point to) {
    selLast_ = to;
    if (worker_ && selecting_) selPending_ = worker_->select(page_, selAnchor_, to);
}

void ReaderScreen::onSelection(const RenderEvent& e, Nav& nav) {
    // Any answer newer than the one shown is drawn: while the finger keeps moving, each answer is
    // overtaken by the next request before it arrives, and waiting for the latest would freeze
    // the selection until the finger stops. The bar waits for the answer to the last request.
    if (!selecting_ || e.requestId <= selShown_ || e.page != page_) return;
    selShown_ = e.requestId;
    const bool latest = e.requestId == selPending_;
    // A PDF long press where there's no text (or on a scan) draws an area instead, from where it
    // was pressed to where the finger is now.
    if (type_ == DocType::Pdf && !selection_ && (!e.selection || e.selection->text.empty())) {
        selecting_ = false;
        if (refuseReadOnly(nav, false)) return;  // no area in a read-only group
        const bool lifted = selReleased_;
        beginArea(selAnchor_, selLast_, nav);
        if (lifted) endArea(selLast_, nav);
        return;
    }
    if (!e.selection) {
        if (!selection_) selecting_ = false;    // the long-press wasn't on text
        else if (latest && !selDragging_ && selReleased_ && !bar_) selectionDone(nav);  // dragged off the text: keep what was selected
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
    for (const Rect& r : selection_->rects) {
        selTouched_ = selTouched_.united(r);
        if (!has(before, r)) nav.damage(r, Waveform::Fast);
    }
    if (latest && !selDragging_ && selReleased_ && !bar_) selectionDone(nav);
}

void ReaderScreen::selectionDone(Nav& nav) {
    if (!selAutoHighlight_) return showBar(nav);
    selAutoHighlight_ = false;
    saveSelection("highlight", "", nav);
}

void ReaderScreen::showBar(Nav& nav) {
    if (!selection_) return;
    PluginEntries* e = nav.pluginEntries();
    const bool more = e && !e->actions("selectionAction").empty();
    // A web snapshot or a read-only group: nothing is highlighted, so no colours and no tag row
    // (Search and More… only); in a group, the first time, a toast says why.
    if (!web_ && !preview_) refuseReadOnly(nav, false);
    const bool canHighlight = previewCanSave() || (!web_ && !readOnly_);
    bar_ = !canHighlight ? std::make_unique<AnnotationBar>(AnnotationBar::Mode::Text, boundsOf(selection_->rects), colour_, more)
                : std::make_unique<AnnotationBar>(AnnotationBar::Mode::Create, boundsOf(selection_->rects), colour_, more,
                                                  TagRow(selectionChips(nav), selTags_));
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
        case AnnotationBar::Action::Tag: {  // marks the tag for the highlight about to be made
            const std::string tag = bar_->tags().chips()[static_cast<size_t>(hit.colour)];
            bar_->toggleTag(tag);
            selTags_ = bar_->tags().on();
            nav.damage(bar_->tags().rect(), Waveform::Ui);
            break;
        }
        case AnnotationBar::Action::AddTag:
            pickAnnotationTag(nav, [this, n = &nav](const std::string& tag) {
                if (!selection_) return;  // gone meanwhile (the page changed)
                if (std::find(selTags_.begin(), selTags_.end(), tag) == selTags_.end()) selTags_.push_back(tag);
                if (bar_) {
                    bar_->tags().setChips(selectionChips(*n, tag));
                    bar_->tags().setOn(selTags_);
                }
            });
            break;
        case AnnotationBar::Action::Highlight:
            saveSelection("highlight", "", nav);
            break;
        case AnnotationBar::Action::Underline:
            saveSelection("underline", "", nav);
            break;
        case AnnotationBar::Action::Search: {
            const std::string query = selection_ ? selection_->text : std::string{};
            closeSelection(nav);
            if (!query.empty()) nav.push(std::make_unique<BookSearchScreen>(*this, query));
            break;
        }
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
            if (!e || !selection_ || (!preview_ && (!item || !att))) break;
            const auto pageLabel = selection_->pageLabel.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(selection_->pageLabel);
            nlohmann::ordered_json target;
            if (preview_) {  // not in the library: no item, no attachment; what it is instead (0.9)
                target = {{"text", selection_->text}, {"item", nullptr}, {"attachment", nullptr}, {"pageLabel", pageLabel},
                          {"preview", {{"title", row_.title}, {"url", preview_->url.empty() ? nlohmann::ordered_json(nullptr)
                                                                                           : nlohmann::ordered_json(preview_->url)}}}};
            } else {
                target = {{"text", selection_->text},
                          {"item", plugins::itemJson(*item, nav.library().isRead(row_.key), nav.library().libraryName(item->lib), nav.library().readingInfo(row_.key))},
                          {"attachment", plugins::attachmentJson(*att)},
                          {"pageLabel", pageLabel}};
            }
            result_ = std::make_unique<PluginResult>(*e, chosen, "selectionAction", std::move(target),
                                                     pageArea());
            bar_->setWorking(true);
            break;
        }
        case AnnotationBar::Action::Note:  // the tags marked stay for the note's highlight (selTags_)
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
    a.tags = selTags_;  // the chips marked on the bar
    if (previewCanSave()) {
        a.key = "preview-ann-" + std::to_string(++previewAnnSeq_);
        const std::string key = a.key;
        previewAnnotations_.push_back(a);
        for (const Rect& r : selection_->rects) {
            HighlightBox b{key, r, shadeOf(colour_), type == "underline"};
            b.color = colour_;
            highlights_.push_back(std::move(b));
        }
        nav.device().set(kColourKey, colour_);
        refreshHighlights(nav);
        closeSelection(nav);
        return;
    }
    for (auto it = selTags_.rbegin(); it != selTags_.rend(); ++it) nav.library().rememberAnnotationTag(*it);
    const std::string key = nav.library().createAnnotation(a);
    // Drawn now, where the selection was (as the page will draw it: Selection::rects): the page
    // coming back with it changes nothing on the screen.
    for (const Rect& r : selection_->rects) {
        HighlightBox b{key, r, shadeOf(colour_), type == "underline"};
        b.color = colour_;
        highlights_.push_back(std::move(b));
    }
    plugins::annotationEvent(nav.plugins(), nav.library(), "highlightCreated", key);
    nav.device().set(kColourKey, colour_);
    if (a.localOnly) nav.toast("Saved on the Kindle only — this place can't be synced exactly.");
    else nav.sync().pushSoon();
    refreshHighlights(nav);
    closeSelection(nav);
}

void ReaderScreen::closeSelection(Nav& nav) {
    if (note_) nav.keyboard().hide();
    // Repainted: every line the selection covered on the way (inverted with the fast waveform, so
    // without ghosts now), the bar and the note box; not the whole page.
    std::vector<Rect> gone{selTouched_};
    if (selection_) gone.insert(gone.end(), selection_->rects.begin(), selection_->rects.end());
    if (bar_) gone.push_back(bar_->rect());
    if (note_) gone.push_back(noteRect());
    dropSelection();
    for (const Rect& r : gone)
        if (!r.empty()) nav.damage(r, Waveform::Quality);
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
    if (image_ && !error_) {  // white only beside and below the page: the page covers the rest
        fillRect(cr, {image_->width, 0, std::max(0, W() - image_->width), H()}, 255);
        fillRect(cr, {0, image_->height, W(), std::max(0, H() - image_->height)}, 255);
    } else {
        fillRect(cr, {0, 0, W(), H()}, 255);
    }
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
    drawHighlights(canvas, highlights_, colorOn_);
    {
        cairo_save(cr);
        const Rect pa = pageArea();
        cairo_rectangle(cr, pa.x, pa.y, pa.w, pa.h);
        cairo_clip(cr);
        for (const auto& s : inkSaved_)
            if (s.page == page_) strokePaths(cr, s.paths, inkWidthPx(), inkRgb(s.colour));
        for (const auto& [id, s] : inkSent_)
            if (s.page == page_) strokePaths(cr, s.paths, inkWidthPx(), inkRgb(s.colour));
        if (inkPage_ == page_) strokePaths(cr, inkPending_, inkWidthPx(), inkRgb(inkColour_));
        if (inkDown_) strokePaths(cr, {inkStroke_}, inkWidthPx(), inkRgb(inkColour_));
        cairo_restore(cr);
    }
    if (selection_) drawSelection(canvas, selection_->rects);
    {  // areas: being drawn (ink), being saved, saved but not yet back with the page
        cairo_save(cr);
        const Rect pa = pageArea();
        cairo_rectangle(cr, pa.x, pa.y, pa.w, pa.h);
        cairo_clip(cr);
        auto frame = [&](Rect r, const std::string& colour) {
            strokeFrame(cr, r, areaFrameShade(shadeOf(colour)), 3, colorOn_ ? std::optional(zoteroRgb(colour)) : std::nullopt);
        };
        for (const auto& [id, s] : areaSent_)
            if (s.page == page_) frame(s.rect, s.colour);
        for (const auto& s : areaSaved_)
            if (s.page == page_) frame(s.rect, s.colour);
        if (auto r = drawnArea()) strokeFrame(cr, *r, kInk, 3);
        if (editor_)  // the area its bar is for: framed in ink while selected
            for (const auto& b : highlights_)
                if (b.kind == HighlightBox::Kind::Image && b.annotationKey == editor_->key()) strokeFrame(cr, b.rect, kInk, 3);
        cairo_restore(cr);
    }
    const Rect f = footerArea();
    const int leftW = W() - 2 * kMargin - 260 - (penControls() ? W() - kMargin - 260 - undoRect().x : 0);
    if (auto b = backLabel())  // after a jump: the way back, where the chapter line was
        drawText(cr, *b, {kSans, 7, 700}, {kMargin, f.y + 10, leftW, 44}, 1);
    else
        drawText(cr, footerLeft(), {kSans, 6.5, 400, false, kTertiary}, {kMargin, f.y + 12, leftW, 40}, 1);
    if (penControls()) {
        auto dot = [&](double cx, double cy, double r, const std::string& colour) {
            const uint32_t v = zoteroRgb(colour);
            if (colorOn_) rgb(cr, v);
            else gray(cr, colour == "#000000" ? kInk : shadeOf(colour));
            cairo_arc(cr, cx, cy, r, 0, 2 * 3.14159265);
            cairo_fill_preserve(cr);
            gray(cr, 120);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        };
        const Rect c = colourChipRect();  // the highlight colour, then the ink's
        dot(c.x + 24, c.y + c.h / 2.0, 17, colour_);
        dot(c.x + 64, c.y + c.h / 2.0, 17, inkColour_);
        if (undoShown_) centredText(cr, "Undo", {kSans, 7, 700}, undoRect());
    }
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
    if (bar_) bar_->draw(cr, colorOn_);
    if (paletteOpen_ && !menus_) {
        const Rect p = paletteRect();
        roundedRect(cr, p.x + 1.5, p.y + 1.5, p.w - 3, p.h - 3, 18);
        gray(cr, 255);
        cairo_fill_preserve(cr);
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        for (int row = 0; row < 2; ++row) {
            const Rect first = paletteDot(row, 0);
            centredText(cr, row ? "Ink" : "Highlight", {kSans, 7.5, 500}, {p.x + 30, first.y, 180, first.h}, Align::Left);
            for (int i = 0; i < (row ? 9 : 8); ++i) {
                const std::string hex = row && i == 0 ? "#000000" : zoteroColours()[row ? i - 1 : i].hex;
                const Rect d = paletteDot(row, i);
                const double cx = d.x + d.w / 2.0, cy = d.y + d.h / 2.0;
                if (colorOn_) rgb(cr, zoteroRgb(hex));
                else gray(cr, hex == "#000000" ? kInk : shadeOf(hex));
                cairo_arc(cr, cx, cy, 30, 0, 2 * 3.14159265);
                cairo_fill_preserve(cr);
                gray(cr, 120);
                cairo_set_line_width(cr, 2);
                cairo_stroke(cr);
                if ((row ? inkColour_ : colour_) == hex) {  // the chosen one: a ring
                    gray(cr, kInk);
                    cairo_set_line_width(cr, 5);
                    cairo_arc(cr, cx, cy, 40, 0, 2 * 3.14159265);
                    cairo_stroke(cr);
                }
            }
        }
    }
    if (notePopup_) notePopup_->draw(cr);
    if (editor_) editor_->draw(cr, colorOn_);
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
        const TextStyle style{kSerif, 9, 400, false, empty ? kTertiary : kInk};
        const Rect text{n.x + 30, n.y + 84, n.w - 60, 110};
        if (empty) drawText(cr, "Type a note…", style, text, 2);
        else drawTextLines(cr, note_->text(), style, text, firstOfLastLines(note_->text(), style, text.w, 2), 2);  // the end, where typing is
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
    if (previewClosePrompt_) previewClosePrompt_->draw(cr);
    if (menus_) menus_->draw(canvas);
}

// --- Getting around: Back, bookmarks, Go to ---------------------------------------------------

void ReaderScreen::markJumpOrigin() {
    pendingGo_.reset();  // this jump instead
    bookmarkArriving_ = false;  // a corner tap was for the page a turn brought, not where this jump goes
    turnTarget_ = -1;  // a jump: where it lands is the worker's to say
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
    pendingGo_.reset();  // Back instead
    bookmarkArriving_ = false;
    commitInk();
    turning_ = true;
    turnTarget_ = -1;
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
    nav.damage(bookmarkRect(), Waveform::Quality);  // a removed ribbon goes now; an added one with the page
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

const LinkBox* ReaderScreen::linkNear(Point p) const {
    if (turning_ || layoutPending_ || links_.empty() || !worker_) return nullptr;
    const Rect page = pageArea();
    const int strip = page.w * 8 / 100;  // the outer strips always turn the page
    if (p.x < page.x + strip || p.x >= page.x + page.w - strip) return nullptr;
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
    return best;
}

bool ReaderScreen::tapLink(Point p, Nav&) {
    const LinkBox* best = linkNear(p);
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
        commitInk();  // its strokes belong to the page it leaves
        turning_ = true;
        worker_->showZotero(remoteValue_);  // placed in the pages as they are now (a PDF page: its first view)
    }
    question_.reset();
    nav.damage(positionBannerRect(), Waveform::Quality);  // the page under it, without ghosts
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
    if (type_ != DocType::Pdf || !ready() || refuseReadOnly(nav, true)) return;
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

void ReaderScreen::dropArea(bool keepNotePopup) {
    areaArmed_ = areaDrawing_ = areaPen_ = noteArmed_ = false;
    if (!keepNotePopup) notePopup_.reset();
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
    nav.device().set(kColourKey, sent.colour);
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
    editor_ = std::make_unique<AnnotationEditor>(*saved, sent.rect, recentChips(nav.library()));
    newAreaKey_ = key;
    nav.damage(editor_->rect(), Waveform::Ui);
}

// --- Sticky notes ----------------------------------------------------------------------------

void ReaderScreen::addNote(Nav& nav) {
    closeMenus(nav);
    if (type_ != DocType::Pdf || !ready() || refuseReadOnly(nav, true)) return;
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
    nav.device().set(kColourKey, colour_);
}

// The nearest note whose icon is within ~9 mm of the tap.
const HighlightBox* ReaderScreen::noteNear(Point p) const {
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
    return best;
}

bool ReaderScreen::openNoteAt(Point p, Nav& nav) {
    const HighlightBox* best = noteNear(p);
    if (!best) return false;
    auto a = nav.library().annotation(best->annotationKey);
    if (!a) return false;
    linkId_ = 0;
    notePopup_ = std::make_unique<NotePopup>(*a, noteIcon(best->rect), pageArea(), recentChips(nav.library()),
                                             annotationReadOnly(nav.library(), *a));
    nav.damage(notePopup_->rect(), Waveform::Ui);
    return true;
}

void ReaderScreen::onNotePopup(const Gesture& g, Nav& nav) {
    const NotePopup::Action act = notePopup_->onGesture(g, nav);
    if (act == NotePopup::Action::None) return;
    if (act == NotePopup::Action::Tag) {  // as the bar's chips: at once, on the note
        const std::string key = notePopup_->key();
        const std::string tag = notePopup_->tags().chips()[static_cast<size_t>(notePopup_->tappedChip())];
        const auto tags = setAnnotationTag(nav, key, tag, !notePopup_->tags().isOn(tag));
        if (!tags) return closeNotePopup(nav);
        if (auto now = nav.library().annotation(key)) notePopup_->setNote(*now);
        notePopup_->tags().setOn(*tags);
        nav.damage(notePopup_->tags().rect(), Waveform::Ui);
        nav.sync().pushSoon();
        return;
    }
    if (act == NotePopup::Action::AddTag) {
        notePicking_ = true;
        pickAnnotationTag(nav, [this, n = &nav, key = notePopup_->key()](const std::string& tag) {
            const auto tags = setAnnotationTag(*n, key, tag, true);
            if (tags) n->sync().pushSoon();
            if (!notePopup_ || notePopup_->key() != key) return;  // closed meanwhile
            if (!tags) {
                notePopup_.reset();
                return;
            }
            if (auto now = n->library().annotation(key)) notePopup_->setNote(*now);
            notePopup_->tags().setChips(recentChips(n->library()));
            notePopup_->tags().setOn(*tags);
        });
        return;
    }
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

void ReaderScreen::closeNotePopup(Nav& nav) {
    if (!notePopup_) return;
    nav.damage(notePopup_->rect(), Waveform::Quality);
    notePopup_.reset();
}

// A new selection's chips: the recent tags, after any tag marked on it that isn't among them
// (one + Tag chose), `first` leading.
std::vector<std::string> ReaderScreen::selectionChips(Nav& nav, const std::string& first) const {
    std::vector<std::string> out;
    auto add = [&](const std::string& t) {
        if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    };
    add(first);
    const std::vector<std::string> recent = nav.library().recentAnnotationTags();
    for (const auto& t : selTags_)
        if (std::find(recent.begin(), recent.end(), t) == recent.end()) add(t);
    for (const auto& t : recent) add(t);
    if (out.size() > TagRow::kChips) out.resize(TagRow::kChips);
    return out;
}

// --- Handwriting -----------------------------------------------------------------------------

bool ReaderScreen::penAllowed() const {
    return worker_ && image_ && !error_ && !menus_ && !selecting_ && !selection_ && !editor_ && !note_ &&
           !areaArmed_ && !areaDrawing_ && !noteArmed_ && !notePopup_ &&
           !layoutPending_ && !turning_ && !linkPopup_ && !paletteOpen_ && !(result_ && result_->shown());
}

bool ReaderScreen::wantsSystemInk() const {
    return type_ == DocType::Pdf && penWrites_ && !readOnly_ && penAllowed() && !eraserNear_ && !buttonHeld_;
}

void ReaderScreen::setPenWidth(int i, Nav& nav) {
    penWidth_ = std::clamp(i, 0, 2);
    nav.device().set(kPenWidthKey, std::to_string(penWidth_));
    nav.setSystemInkWidth(inkWidthPx());
}

void ReaderScreen::setPenButtonHighlights(bool on, Nav& nav) {
    penButtonHighlights_ = on;
    nav.device().set(kPenButtonKey, on ? "highlight" : "");
}

void ReaderScreen::setPenWrites(bool on, Nav& nav) {
    penWrites_ = on;
    nav.device().set(kPenModeKey, on ? "" : "tap");
    if (nav.systemInk()) nav.setSystemInk(wantsSystemInk());
}

bool ReaderScreen::onPen(const PenEvent& p, Nav& nav) {
    penSeen_ = true;
    lastPenMs_ = nav.nowMs();
    if (palmTap_) {  // a page-turn tap just before the pen came: the hand settling to write
        palmTap_.reset();
        log(LogLevel::Info, "reader: a tap just before the pen taken for the palm (no page turn)");
    }
    // A selection the finger is still holding (or still waiting for the worker to show) when the
    // pen comes: the palm rested on the page before the pen was near. Dropped, so the pen writes.
    // Where there was no text it became an area being drawn (onSelection).
    if (fingerSelecting_ && !penSelecting_ &&
        ((selecting_ && (!selReleased_ || !selection_)) || (areaDrawing_ && !areaPen_ && !areaArmed_))) {
        log(LogLevel::Info, "reader: a selection held when the pen came taken for the palm");
        if (areaDrawing_) cancelArea(nav);
        closeSelection(nav);
        if (nav.systemInk() != wantsSystemInk()) nav.setSystemInk(wantsSystemInk());
    }
    fingerSelecting_ = false;
    if (!penOnKindle_) {
        penOnKindle_ = true;
        nav.device().set(kPenOnKindleKey, "1");
        if (!menus_) nav.damage(footerArea(), Waveform::Ui);  // the pen controls come
    }
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
    if (penSelecting_) {  // a side-button stroke (Highlight): the selection follows the pen
        if (p.type == PenEvent::Type::Move) {
            if (areaDrawing_) moveArea(p.pos, nav);  // no text where it began: an area, as with a finger
            else requestSelection(p.pos);
        }
        if (lift) {
            penSelecting_ = false;
            if (areaDrawing_) {
                endArea(p.pos, nav);
            } else if (selecting_) {
                selDragging_ = false;
                selReleased_ = true;
                requestSelection(p.pos);  // its answer saves the highlight (selectionDone)
            }
        }
        return true;
    }
    if (lift && wipeFrom_) {
        nav.damage(footerArea().contains(*wipeFrom_) ? footerArea() : Rect{0, 0, W(), H()}, Waveform::Ui);
        wipeFrom_.reset();
    }
    if (lift) {
        if (inkDown_) {
            inkDown_ = false;
            // In writing mode the pen only writes (dots included): it never turns into a tap
            // that opens annotations, links, bookmarks, menus, or turns pages.
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
    // Side button held, set to Highlight: the stroke selects from where it starts (no long press)
    // and the selection is saved as a highlight in the current colour when the pen lifts.
    if (p.type == PenEvent::Type::Down && p.button && penButtonHighlights_ && ready() && penAllowed() &&
        pageArea().contains(p.pos) && !(question_ && positionBannerRect().contains(p.pos))) {
        startSelection(p.pos, nav);
        selDragging_ = true;  // nothing is saved before the pen lifts
        selAutoHighlight_ = (!readOnly_ && !web_) || previewCanSave();  // else the bar, which says why
        penSelecting_ = true;
        return true;
    }
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
        // A read-only group: no ink, nothing erased; the pen taps (the first time, a toast says why).
        const bool readOnly = type_ == DocType::Pdf && penWrites_ && !p.button && penAllowed() &&
                              pageArea().contains(p.pos) && !onBanner && refuseReadOnly(nav, false);
        if (type_ != DocType::Pdf || !penWrites_ || p.button || !penAllowed() || !pageArea().contains(p.pos) ||
            onBanner || readOnly) {
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
        const Rect r{p.pos.x - inkWidthPx(), p.pos.y - inkWidthPx(), 2 * inkWidthPx() + 1, 2 * inkWidthPx() + 1};
        if (!nav.systemInk())  // the system's wet ink already shows the line
            nav.paintNow(r.intersected(pageArea()), Waveform::Ink,
                     [&](cairo_t* cr) { strokePaths(cr, {{p.pos}}, inkWidthPx(), inkRgb(inkColour_)); });
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
    const int pad = inkWidthPx() + 1;
    const Rect r{std::min(a.x, b.x) - pad, std::min(a.y, b.y) - pad, std::abs(a.x - b.x) + 2 * pad + 1,
                 std::abs(a.y - b.y) + 2 * pad + 1};
    nav.paintNow(r.intersected(pageArea()), Waveform::Ink,
                 [&](cairo_t* cr) { strokePaths(cr, {{a, b}}, inkWidthPx(), inkRgb(inkColour_)); });
    return true;
}

void ReaderScreen::commitInk() {
    if (inkPending_.empty() || !worker_ || inkPage_ < 0) return;
    const uint64_t id = worker_->ink(inkPage_, inkPending_, inkWidthPx());
    inkSent_[id] = SentInk{inkPage_, std::move(inkPending_), false, inkColour_, ++inkSeq_};
    inkPending_.clear();
}

void ReaderScreen::flushInk(Nav& nav, int64_t timeoutMs) {
    commitInk();
    if (inkFlushGaveUp_) timeoutMs = 0;
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    // Areas and notes just made are placed by the worker too: lost the same way if not waited for.
    auto waiting = [&] { return inkSent_.size() + areaSent_.size() + noteSent_.size(); };
    pumpEvents(nav, false);
    while (waiting() > 0 && clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pumpEvents(nav, false);  // the Ink and Area answers make the annotations
    }
    if (waiting() > 0) {
        inkFlushGaveUp_ = true;  // the close's second flush doesn't wait again
        // Each kind lost is told once: a later flush can lose a kind the first didn't.
        if (!inkSent_.empty() && !toldInkLost_) {
            toldInkLost_ = true;
            log(LogLevel::Warn, "reader: handwriting not placed in time, %zu request(s) lost", inkSent_.size());
            nav.toast("Some handwriting couldn't be saved.");
        }
        if ((!areaSent_.empty() || !noteSent_.empty()) && !toldAreaLost_) {
            toldAreaLost_ = true;
            log(LogLevel::Warn, "reader: areas or notes not placed in time, %zu request(s) lost", areaSent_.size() + noteSent_.size());
            nav.toast("An area or note couldn't be saved.");  // as onArea's failures
        }
    }
}

void ReaderScreen::onInk(const RenderEvent& e, Nav& nav) {
    auto it = inkSent_.find(e.requestId);
    if (it == inkSent_.end()) return;
    SentInk sent = std::move(it->second);
    inkSent_.erase(it);
    if (sent.paths.empty()) return;  // all of it erased while it was being placed
    if (sent.erased) {               // placed with strokes since erased: place what is left
        const uint64_t id = worker_->ink(sent.page, sent.paths, inkWidthPx());
        inkSent_[id] = SentInk{sent.page, std::move(sent.paths), false, sent.colour, sent.seq};
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
    a.color = sent.colour;
    a.position = e.ink->position;
    a.sortIndex = e.ink->sortIndex;
    a.pageLabel = e.ink->pageLabel;
    const std::string key = nav.library().createAnnotation(a);
    plugins::annotationEvent(nav.plugins(), nav.library(), "highlightCreated", key);
    nav.sync().pushSoon();
    inkUndo_.push_back(UndoInk{key, sent.page, sent.seq});
    inkSaved_.push_back(SavedInk{key, sent.page, std::move(sent.paths), sent.colour});
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
    const double radius = inkWidthPx() / 2.0 + 12;
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
        if (!nav.library().annotationEditable(*rec)) continue;  // another member's ink (or a read-only group's): theirs
        const auto& [crossed, strokes] = h;
        if (crossed.size() < strokes) {
            nlohmann::json pos = nlohmann::json::parse(rec->position, nullptr, false);
            if (pos.is_discarded() || !pos.contains("paths") || pos["paths"].size() != strokes) continue;
            nlohmann::json kept = nlohmann::json::array();
            for (size_t i = 0; i < strokes; ++i)
                if (!crossed.count(i)) kept.push_back(pos["paths"][i]);
            pos["paths"] = kept;
            rec->position = pos.dump();
            if (!nav.library().editAnnotation(*rec)) continue;
            plugins::annotationEvent(nav.plugins(), nav.library(), "highlightChanged", key);
        } else if (!rec->comment.empty() || !rec->tags.empty()) {
            nav.toast("This handwriting has a comment or tags: delete it in Notes.");
            continue;
        } else {
            const bool told = nav.plugins().listening();
            const plugins::AnnotationRef ref = told ? plugins::annotationRef(nav.library(), *rec) : plugins::AnnotationRef{};
            if (!nav.library().deleteAnnotation(key)) continue;
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
