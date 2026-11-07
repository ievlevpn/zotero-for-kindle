#pragma once
// What the UI thread may ask of the background sync, and what it hears back (spec §7.2).
// SyncService (src/sync/sync_service.h) implements it; tests use tests/fake_sync.h.

#include <cstdint>
#include <optional>
#include <string>

#include "net/errors.h"
#include "sync/sync_engine.h"

namespace zk {

struct SyncEvent {
    enum class Kind {
        SyncStarted, SyncProgress, SyncFinished, DownloadProgress, DownloadFinished, AnnotationsReady,
        PushStarted,  // `total` changes going up
        PushFinished  // report.annotations sent; error: they stay unsent
    };
    Kind kind = Kind::SyncStarted;
    SyncProgress progress;
    SyncReport report;
    std::string itemKey, attachmentKey;
    int64_t done = 0, total = 0;
    std::optional<UserError> error;
};

class SyncControl {
public:
    virtual ~SyncControl() = default;
    virtual void syncNow() = 0;  // sends unsent changes, pulls, then sends again
    // Send the Kindle's annotation changes a little after the last one (debounced, spec §7.4).
    virtual void pushSoon() = 0;
    // Sync once Wi-Fi is up (after wake): the worker waits for it, the UI thread never probes it.
    virtual void syncWhenOnline() = 0;
    // Download the file if needed, then its annotations (on demand, spec §7.4).
    virtual void openBook(const std::string& itemKey, const std::string& attachmentKey) = 0;
    virtual void setAllHighlightsOffline(bool) = 0;
    virtual void removeDownloads() = 0;
    virtual void setIntervalMinutes(int minutes /* 0 = off */) = 0;
    virtual bool poll(SyncEvent& out) = 0;  // non-blocking
    virtual bool busy() const = 0;
};

}  // namespace zk
