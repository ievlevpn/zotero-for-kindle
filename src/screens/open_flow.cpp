#include "screens/open_flow.h"

#include "screens/book.h"
#include "screens/reader.h"

namespace zk {

std::string OpenFlow::statusFor(const std::string& key) const {
    auto it = pending_.find(key);
    return it == pending_.end() ? std::string{} : it->second.status;
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
    if (row.attachmentKey.empty()) {
        bool linked = false;
        for (const auto& a : nav.library().attachmentsOf(row.key))
            linked |= a.linkMode == "linked_file" &&
                      (a.contentType == "application/epub+zip" || a.contentType == "application/pdf");
        nav.toast(linked ? "This file is only on your computer." : "No EPUB or PDF for this item.");
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
        Pending{row, nav.top(), nav.sync().busy() ? "Waiting for sync to finish…" : "Downloading…", true, std::move(at)};
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
            ItemRow fresh = nav.library().rowFor(row.key).value_or(row);
            openNow(fresh, nav, at);
        }
    }
}

}  // namespace zk
