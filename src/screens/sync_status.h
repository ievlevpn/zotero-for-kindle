#pragma once
// What the header shows about sync: spinning/idle, "Syncing… 1,200 of 6,560", "Synced 5 min
// ago", "Lab: can't sync" (a group's pass failed, the rest synced; Settings → Group libraries says
// why), and the last error (shown as a banner). Fed with every SyncEvent.

#include <cstdint>
#include <optional>
#include <string>

#include <cairo.h>

#include "core/geometry.h"
#include "sync/sync_control.h"

namespace zk {

class SyncStatus {
public:
    void onEvent(const SyncEvent&, int64_t nowMs);
    void reset() {  // a new sync service: nothing is running
        syncing_ = false;
        progress_ = {};
    }
    bool syncing() const { return syncing_ || uploading_ > 0; }
    bool hasError() const { return error_.has_value() || pushError_.has_value(); }
    const UserError& error() const { return error_ ? *error_ : *pushError_; }
    std::string line(int64_t nowMs) const;
    int64_t msSinceSync(int64_t nowMs) const;  // INT64_MAX before the first sync
    void drawIndicator(cairo_t*, Rect) const;

private:
    bool syncing_ = false;
    SyncProgress progress_;
    int64_t lastFinishedMs_ = -1;
    std::optional<UserError> error_;
    int64_t uploading_ = 0;                // changes going up now
    std::optional<UserError> pushError_;  // the last upload failed: its changes are still unsent
    std::string groupProblem_;            // the last sync: "Lab: can't sync" (My Library fine)
};

// 6560 -> "6,560".
std::string thousands(int64_t);

}  // namespace zk
