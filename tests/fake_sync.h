#pragma once
// Records what the UI asked of the sync service and lets tests feed it events.

#include "sync/sync_control.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace zk::test {

class FakeSync : public SyncControl {
public:
    void syncNow() override { ++syncs; }
    uint64_t syncNowTracked() override {  // ids from 100, as SyncEvent::request carries them
        ++syncs;
        return lastRequest = 100 + static_cast<uint64_t>(syncs);
    }
    void pushSoon() override { ++pushes; }
    void syncWhenOnline() override { ++whenOnline; }
    void openBook(const std::string& itemKey, const std::string& attachmentKey) override {
        opened.push_back(itemKey + "/" + attachmentKey);
    }
    void setAllHighlightsOffline(bool on) override { allOffline.push_back(on); }
    void removeDownloads() override { ++removals; }
    void setIntervalMinutes(int m) override { interval = m; }
    bool poll(SyncEvent& out) override {
        if (events.empty()) return false;
        out = events.front();
        events.pop_front();
        return true;
    }
    bool busy() const override { return isBusy; }
    void downloadKept() override { ++keptRuns; }
    std::optional<KeptStatus> keptStatus(const std::string& key) const override {
        auto it = kept.find(key);
        return it == kept.end() ? std::nullopt : std::optional(it->second);
    }

    void removeLibrary(int64_t lib, bool evenUnsent) override { removedLibraries.push_back({lib, evenUnsent}); }

    uint64_t lastRequest = 0;  // the last syncNowTracked()'s id
    int syncs = 0, pushes = 0, whenOnline = 0, removals = 0, interval = -1, keptRuns = 0;
    std::vector<std::pair<int64_t, bool>> removedLibraries;  // removeLibrary(lib, evenUnsent), in order
    std::map<std::string, KeptStatus> kept;  // what keptStatus() answers
    bool isBusy = false;
    std::vector<std::string> opened;
    std::vector<bool> allOffline;
    std::deque<SyncEvent> events;
};

}  // namespace zk::test
