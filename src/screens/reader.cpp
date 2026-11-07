#include "screens/reader.h"

#include <cmath>

#include <nlohmann/json.hpp>

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

}  // namespace

ReaderScreen::ReaderScreen(ItemRow row, std::string path, DocType type, WorkerFactory makeWorker, int flashEvery,
                           Lipc* lipc)
    : row_(std::move(row)), path_(std::move(path)), type_(type), makeWorker_(std::move(makeWorker)),
      flashEvery_(flashEvery > 0 ? flashEvery : 6), lipc_(lipc) {}

ReaderScreen::~ReaderScreen() = default;

Rect ReaderScreen::pageArea() { return {0, 0, kW, kH - kFooterH}; }
Rect ReaderScreen::footerArea() { return {0, kH - kFooterH, kW, kFooterH}; }

bool ReaderScreen::wantsKeyboard() const { return false; }

void ReaderScreen::onShow(Nav& nav) {
    if (worker_) return;  // back from a pushed panel: the book stays open
    layout_ = loadLayout(nav.library(), row_.key);
    DocPosition start;
    if (auto st = nav.library().readingState(row_.key)) start.value = st->location;
    worker_ = makeWorker_();
    worker_->open(path_, type_, layout_, start);
    worker_->highlightsFor(nav.library().annotationsFor(row_.attachmentKey));
}

void ReaderScreen::onTick(Nav& nav) {
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
                nav.redrawAll(Waveform::Quality);
                break;
            case RenderEvent::Kind::SearchResults:
                if (menus_) menus_->onSearchResults(e.hits, nav);
                break;
            case RenderEvent::Kind::Page: {
                const bool first = image_ == nullptr;
                const bool chapterChanged = !first && e.chapter != chapter_;
                const bool turned = turning_;
                image_ = e.image;
                highlights_ = e.highlights;
                page_ = e.page;
                pageCount_ = e.pageCount;
                chapter_ = e.chapter;
                progress_ = e.progress;
                wordsLeft_ = e.wordsLeftInChapter;
                position_ = e.position;
                turning_ = false;
                nav.library().saveReading(row_.key, position_.value, progress_);
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
    if (menus_) menus_->onTick(nav);
}

void ReaderScreen::turn(int delta, Nav&) {
    if (!worker_ || page_ < 0) return;
    const int target = page_ + delta;
    if (target < 0 || (pageCount_ > 0 && target >= pageCount_)) return;
    turning_ = true;
    worker_->show(target);
}

void ReaderScreen::turnTo(int page) {
    if (!worker_) return;
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
    if (l == layout_ || !worker_) return;
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
    if (g.type == GestureType::Swipe) {
        if (g.dir == Direction::Left) turn(+1, nav);
        if (g.dir == Direction::Right) turn(-1, nav);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (g.pos.x > kW * 7 / 10) turn(+1, nav);
    else if (g.pos.x < kW * 3 / 10) turn(-1, nav);
    else openMenus(nav);
}

void ReaderScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (menus_) menus_->onKey(k, nav);
}

void ReaderScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind == SyncEvent::Kind::AnnotationsReady && e.attachmentKey == row_.attachmentKey && worker_)
        worker_->highlightsFor(nav.library().annotationsFor(row_.attachmentKey));
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
    const Rect f = footerArea();
    drawText(cr, footerLeft(), {kSans, 6.5, 400, false, kTertiary}, {kMargin, f.y + 12, kW - 2 * kMargin - 260, 40}, 1);
    drawText(cr, footerRight(), {kSans, 6.5, 400, false, kTertiary}, {kW - kMargin - 260, f.y + 12, 260, 40}, 1, Align::Right);
    if (menus_) menus_->draw(canvas);
}

}  // namespace zk
