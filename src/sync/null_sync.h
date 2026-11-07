#pragma once
// A sync that does nothing: before an account is set up, and for screenshots.

#include "sync/sync_control.h"

namespace zk {

class NullSync final : public SyncControl {
public:
    void syncNow() override {}
    void syncWhenOnline() override {}
    void openBook(const std::string&, const std::string&) override {}
    void setAllHighlightsOffline(bool) override {}
    void removeDownloads() override {}
    void setIntervalMinutes(int) override {}
    bool poll(SyncEvent&) override { return false; }
    bool busy() const override { return false; }
};

}  // namespace zk
