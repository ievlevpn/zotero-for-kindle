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
        PushFinished,  // report.annotations sent; error: they stay unsent
        KeptChanged    // a kept collection's catch-up moved on, paused or ended: see keptStatus()
    };
    Kind kind = Kind::SyncStarted;
    SyncProgress progress;
    SyncReport report;
    std::string itemKey, attachmentKey;
    int64_t done = 0, total = 0;
    std::optional<UserError> error;
};

// A kept collection catching up (spec B): "Downloading <current> of <total>", or paused because
// the Kindle is nearly full. None when it has everything (or can't go on without Wi-Fi).
struct KeptStatus {
    int current = 0, total = 0;
    bool paused = false;
    bool operator==(const KeptStatus&) const = default;
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
    // Kept collections (Library::keptCollections): download what they miss, one file at a time,
    // if Wi-Fi is up (never turned on for it). A sync does this by itself when it ends.
    virtual void downloadKept() {}
    virtual std::optional<KeptStatus> keptStatus(const std::string& /*collectionKey*/) const { return std::nullopt; }
    // Removes a group library's copy from the Kindle (Library::removeLibraryData: its files, items,
    // annotations; the user said yes), on the worker: deleting many files can take a while. Its
    // kept and pinned collections stop being so. A SyncFinished follows (screens reload).
    virtual void removeLibrary(int64_t /*lib*/, bool /*evenUnsent*/) {}
};

}  // namespace zk
