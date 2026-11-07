#pragma once
// Zotero's own file storage, through the Web API: GET /users/<id>/items/<key>/file answers with
// a redirect to a short-lived storage link, which is fetched without the API key. The file comes
// as it is (not zipped); its MD5 is checked against the attachment's. Downloads only.

#include "net/http.h"
#include "sync/file_store.h"

#include <cstdint>
#include <functional>
#include <string>

namespace zk {

class ZoteroStorage final : public FileStore {
public:
    ZoteroStorage(Http&, int64_t userId, std::string apiKey);
    bool download(const AttachmentRecord&, const std::string& destDir, UserError*) override;
    void setProgress(std::function<void(int64_t done, int64_t total)> f) override { progress_ = std::move(f); }
    // Free bytes at a path (statvfs by default); tests inject their own.
    void setFreeSpaceProbe(std::function<uint64_t(const std::string& path)> f) { freeSpace_ = std::move(f); }

private:
    Http& http_;
    int64_t userId_;
    std::string apiKey_;
    std::function<uint64_t(const std::string&)> freeSpace_;
    std::function<void(int64_t, int64_t)> progress_;
};

}  // namespace zk
