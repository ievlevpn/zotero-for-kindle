#include "screens/book.h"

#include <filesystem>

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
constexpr int kTopH = 420;  // header + cover block

std::string sizeText(uintmax_t bytes) {
    if (bytes >= 1024 * 1024) return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
    return std::to_string(std::max<uintmax_t>(1, bytes / 1024)) + " KB";
}

}  // namespace

BookScreen::BookScreen(ItemRow row) : row_(std::move(row)) {}

Rect BookScreen::listArea() const { return {0, kTopH, W(), H() - kTabBarH - kTopH}; }

void BookScreen::load(Library& lib) {
    all_ = lib.annotationsFor(row_.attachmentKey);
    loading_ = all_.empty() && !lib.isTracked(row_.attachmentKey) && !lib.allAnnotationsOffline();
    fileLine_ = row_.badge;
    if (auto a = lib.attachment(row_.attachmentKey)) {
        std::error_code ec;
        if (a->state == AttachmentRecord::State::Local) {
            const auto size = std::filesystem::file_size(a->localPath, ec);
            if (!ec) fileLine_ += " · " + sizeText(size);
        } else {
            fileLine_ += " · not downloaded";
        }
    }
    const int perPage = (listArea().h - kPageIndicatorH) / kNoteRowH;
    const int64_t offset = pager_.offset();
    pager_ = Pager(static_cast<int64_t>(all_.size()), perPage);
    while (pager_.offset() < offset && pager_.next()) {
    }
}

void BookScreen::onShow(Nav& nav) { load(nav.library()); }

void BookScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "", true);
    const Rect cover{kMargin, 110, 210, 290};
    const size_t dot = row_.byline.find(" · ");
    generatedCover(cr, cover, row_.title.empty() ? "Untitled" : row_.title,
                   dot == std::string::npos ? row_.byline : row_.byline.substr(0, dot), true);
    const int x = cover.x + cover.w + 40, w = W() - kMargin - x;
    TextResult t = drawText(cr, row_.title.empty() ? "Untitled" : row_.title, {kSerif, 12, 700}, {x, 116, w, 150}, 3);
    drawText(cr, row_.byline, {kSans, 8, 400, false, kSecondary}, {x, 116 + t.height + 12, w, 50}, 1);
    drawText(cr, fileLine_, {kSans, 7, 400, false, kTertiary}, {x, 116 + t.height + 62, w, 46}, 1);
    hRule(cr, kTopH - 12, 0, W());

    const Rect area = listArea();
    if (loading_) {
        drawText(cr, "Loading highlights…", {kSerif, 10, 400, false, kSecondary}, {kMargin, area.y + 120, W() - 2 * kMargin, 80},
                 1, Align::Center);
        return;
    }
    if (error_ && all_.empty()) {
        drawBanner(cr, {0, area.y + 40, W(), 150}, UserError{error_->kind, "Couldn't load the highlights: " + error_->message,
                                                              error_->details});
        return;
    }
    if (all_.empty()) {
        drawText(cr, "No highlights in this book yet.", {kSerif, 10, 400, false, kSecondary},
                 {kMargin, area.y + 120, W() - 2 * kMargin, 80}, 1, Align::Center);
        return;
    }
    for (int i = 0; i < pager_.perPage() && pager_.offset() + i < static_cast<int64_t>(all_.size()); ++i)
        drawNoteRow(cr, {kMargin, area.y + i * kNoteRowH, W() - 2 * kMargin, kNoteRowH},
                    all_[static_cast<size_t>(pager_.offset() + i)]);
    drawPageIndicator(cr, {0, area.y + area.h - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void BookScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev()))
        nav.damage(listArea(), Waveform::Quality);
}

void BookScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.attachmentKey != row_.attachmentKey && e.kind != SyncEvent::Kind::SyncFinished) return;
    if (e.kind == SyncEvent::Kind::AnnotationsReady) {
        load(nav.library());
        loading_ = false;
        error_ = e.error;
        nav.damage(listArea(), Waveform::Quality);
    } else if (e.kind == SyncEvent::Kind::SyncFinished) {
        const bool wasLoading = loading_;
        load(nav.library());
        loading_ = loading_ && wasLoading;
        nav.damage(listArea(), Waveform::Quality);
    }
}

}  // namespace zk
