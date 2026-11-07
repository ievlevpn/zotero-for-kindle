#pragma once
// What tapping an item does (spec §6.2): open it if the file is here; otherwise download it
// with progress shown in its row, and open it when done if the reader is still on that screen.
// Errors stay in the row ("message (details)"); tapping again retries.

#include <functional>
#include <map>
#include <string>

#include "screens/screen.h"

namespace zk {

class OpenFlow {
public:
    explicit OpenFlow(std::function<int64_t()> wallMs) : wallMs_(std::move(wallMs)) {}

    void open(const ItemRow&, Nav&);
    void onSyncEvent(const SyncEvent&, Nav&);  // from App::setEventHook
    std::string statusFor(const std::string& attachmentKey) const;
    void reset() { pending_.clear(); }
    // Opens a downloaded book in the reader; returns nullptr when it can't (then the Book screen).
    using ReaderFactory = std::function<std::unique_ptr<Screen>(const ItemRow&, Nav&)>;
    void setReaderFactory(ReaderFactory f) { readerFactory_ = std::move(f); }  // a new sync service: forget downloads the old one had
    StatusFn statusFn() const {
        return [this](const std::string& k) { return statusFor(k); };
    }

private:
    struct Pending {
        ItemRow row;
        const Screen* from = nullptr;
        std::string status;
        bool running = false;
    };
    void openNow(const ItemRow&, Nav&);

    std::function<int64_t()> wallMs_;
    ReaderFactory readerFactory_;
    std::map<std::string, Pending> pending_;  // by attachment key
};

}  // namespace zk
