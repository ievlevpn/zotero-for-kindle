#include "screens/sync_status.h"

#include <climits>
#include <cmath>

#include "ui/paint.h"
#include "ui/theme.h"

namespace zk {

std::string thousands(int64_t n) {
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return n < 0 ? "-" + s : s;
}

void SyncStatus::onEvent(const SyncEvent& e, int64_t nowMs) {
    switch (e.kind) {
        case SyncEvent::Kind::SyncStarted:
            syncing_ = true;
            progress_ = {};
            break;
        case SyncEvent::Kind::SyncProgress:
            syncing_ = true;
            progress_ = e.progress;
            break;
        case SyncEvent::Kind::SyncFinished:
            syncing_ = false;
            lastFinishedMs_ = nowMs;
            error_ = e.error;
            break;
        default: break;
    }
}

std::string SyncStatus::line(int64_t nowMs) const {
    if (syncing_)
        return progress_.total > 0 ? "Syncing… " + thousands(progress_.done) + " of " + thousands(progress_.total)
                                   : "Syncing…";
    if (lastFinishedMs_ < 0 || error_) return {};
    const int64_t minutes = (nowMs - lastFinishedMs_) / 60'000;
    return minutes < 1 ? "Synced just now" : "Synced " + std::to_string(minutes) + " min ago";
}

int64_t SyncStatus::msSinceSync(int64_t nowMs) const { return lastFinishedMs_ < 0 ? INT64_MAX : nowMs - lastFinishedMs_; }

void SyncStatus::drawIndicator(cairo_t* cr, Rect r) const {
    fillRect(cr, r, 255);
    const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    if (syncing_) {
        gray(cr, theme::kTertiary);
        cairo_set_line_width(cr, 5);
        cairo_arc(cr, cx, cy, 20, 0, 2 * M_PI);
        cairo_stroke(cr);
        // A short arc of ink shows activity without animating (each frame would be a refresh).
        gray(cr, theme::kInk);
        cairo_arc(cr, cx, cy, 20, -M_PI / 2, M_PI / 3);
        cairo_stroke(cr);
    } else {
        syncIcon(cr, cx, cy, 20);
    }
    if (error_) {
        // A small "!" badge to the lower right, clear of the arrow.
        const int bx = static_cast<int>(cx) + 16, by = static_cast<int>(cy) + 10;
        gray(cr, 255);
        cairo_arc(cr, bx + 14, by + 14, 17, 0, 2 * M_PI);
        cairo_fill(cr);
        gray(cr, theme::kInk);
        cairo_arc(cr, bx + 14, by + 14, 14, 0, 2 * M_PI);
        cairo_fill(cr);
        centredText(cr, "!", {theme::kSans, 6, 700, false, 255}, {bx, by, 28, 28});
    }
}

}  // namespace zk
