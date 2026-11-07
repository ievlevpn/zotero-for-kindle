#pragma once
// Records what the UI asked of the sync service and lets tests feed it events.

#include "sync/sync_control.h"

#include <deque>
#include <string>
#include <vector>

namespace zk::test {

class FakeSync : public SyncControl {
public:
    void syncNow() override { ++syncs; }
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

    int syncs = 0, removals = 0, interval = -1;
    bool isBusy = false;
    std::vector<std::string> opened;
    std::vector<bool> allOffline;
    std::deque<SyncEvent> events;
};

}  // namespace zk::test
