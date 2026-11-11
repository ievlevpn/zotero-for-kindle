#pragma once
// What tapping an item does (spec §6.2): open it if the file is here; otherwise download it
// with progress shown in its row, and open it when done if the reader is still on that screen.
// Errors stay in the row ("message (details)"); tapping again retries. An item with more than one
// readable file asks which first ("Open which file?"), unless the row names its file (exactFile);
// the chosen file's progress shows in the item's row all the same.

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "screens/screen.h"

namespace zk {

class OpenFlow {
public:
    explicit OpenFlow(std::function<int64_t()> wallMs) : wallMs_(std::move(wallMs)) {}

    void open(const ItemRow&, Nav&, std::optional<AnnotationRecord> at = std::nullopt);  // at: show this highlight
    void onSyncEvent(const SyncEvent&, Nav&);  // from App::setEventHook
    // By the file downloading, or by the row's file of an item whose other file is.
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
        std::optional<AnnotationRecord> at;
        std::string listKey;  // the attachment key of the row it was tapped on
    };
    void start(const ItemRow&, Nav&, std::optional<AnnotationRecord> at, const std::string& listKey);
    void openNow(const ItemRow&, Nav&, const std::optional<AnnotationRecord>& at = std::nullopt);

    std::function<int64_t()> wallMs_;
    ReaderFactory readerFactory_;
    std::map<std::string, Pending> pending_;  // by attachment key
};

}  // namespace zk
