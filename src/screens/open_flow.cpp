#include "screens/open_flow.h"

#include "screens/book.h"
#include "screens/file_choice.h"
#include "screens/reader.h"

namespace zk {

std::string OpenFlow::statusFor(const std::string& key) const {
    auto it = pending_.find(key);
    if (it != pending_.end()) return it->second.status;
    for (const auto& [k, p] : pending_)
        if (!key.empty() && p.listKey == key) return p.status;
    return {};
}

void OpenFlow::openNow(const ItemRow& row, Nav& nav, const std::optional<AnnotationRecord>& at) {
    nav.library().markOpened(row.key, row.attachmentKey, wallMs_());
    if (readerFactory_)
        if (auto reader = readerFactory_(row, nav)) {
            if (at)
                if (auto* r = dynamic_cast<ReaderScreen*>(reader.get())) r->jumpToAnnotation(*at);
            nav.push(std::move(reader));
            return;
        }
    nav.push(std::make_unique<BookScreen>(row));
}

void OpenFlow::open(const ItemRow& row, Nav& nav, std::optional<AnnotationRecord> at) {
    if (row.files > 1 && !row.exactFile) {
        auto files = nav.library().fileChoices(row.key);
        if (files.size() > 1) {
            const std::string listKey = row.attachmentKey;
            nav.push(std::make_unique<FileChoiceScreen>(
                row, std::move(files), const_cast<Screen*>(nav.top()),
                [this, at, listKey](const ItemRow& r, Nav& n) { start(r, n, at, listKey); }));
            return;
        }
    }
    start(row, nav, std::move(at), row.attachmentKey);
}

void OpenFlow::start(const ItemRow& row, Nav& nav, std::optional<AnnotationRecord> at, const std::string& listKey) {
    if (row.attachmentKey.empty()) {
        bool linked = false;
        for (const auto& a : nav.library().attachmentsOf(row.key))
            linked |= a.linkMode == "linked_file" &&
                      isReadableType(a.contentType);
        nav.toast(linked ? "This file is only on your computer." : "No EPUB, PDF or saved web page for this item.");
        return;
    }
    auto it = pending_.find(row.attachmentKey);
    if (it != pending_.end() && it->second.running) return;  // already downloading
    if (row.fileState == AttachmentRecord::State::Local) {
        pending_.erase(row.attachmentKey);
        openNow(row, nav, at);
        nav.sync().openBook(row.key, row.attachmentKey);  // refreshes its highlights
        return;
    }
    pending_[row.attachmentKey] =
        Pending{row, nav.top(), nav.sync().busy() ? "Waiting for sync to finish…" : "Downloading…", true, std::move(at), listKey};
    nav.sync().openBook(row.key, row.attachmentKey);
}

void OpenFlow::onSyncEvent(const SyncEvent& e, Nav& nav) {
    auto it = pending_.find(e.attachmentKey);
    if (it == pending_.end()) return;
    Pending& p = it->second;
    if (e.kind == SyncEvent::Kind::DownloadProgress) {
        p.status = e.total > 0 ? "Downloading " + std::to_string(100 * e.done / e.total) + "%" : "Downloading…";
    } else if (e.kind == SyncEvent::Kind::DownloadFinished) {
        p.running = false;
        if (e.error) {
            p.status = e.error->message + (e.error->details.empty() ? "" : " (" + e.error->details + ")");
            return;
        }
        const ItemRow row = p.row;
        const auto at = p.at;
        const bool stillThere = p.from && nav.top() == p.from;
        pending_.erase(it);
        if (stillThere) {
            ItemRow fresh = nav.library().rowFor(row.key, row.attachmentKey).value_or(row);
            openNow(fresh, nav, at);
        }
    }
}

}  // namespace zk
