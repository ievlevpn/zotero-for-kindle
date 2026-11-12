#pragma once
// Zotero's WebDAV file storage: each attachment is <key>.zip (the file) plus <key>.prop (XML
// with mtime and hash) in the configured zotero/ folder. Downloads only, GET only.

#include "net/errors.h"
#include "sync/file_store.h"
#include "net/http.h"
#include "store/records.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace zk {

struct PropInfo {
    int64_t mtime = 0;  // milliseconds since the epoch
    std::string hash;   // MD5 of the file, hex
};

class WebDav final : public FileStore {
public:
    WebDav(Http&, std::string baseUrl /* zotero/ added if missing */, std::string user, std::string password);

    std::optional<PropInfo> prop(const std::string& key, UserError*);
    // Can we log in and list the folder? (PROPFIND, Depth 0.) For the Setup screen.
    bool check(UserError*);
    // Called while the .zip downloads (bytes done, total; total 0 = unknown).
    void setProgress(std::function<void(int64_t done, int64_t total)> f) override { progress_ = std::move(f); }
    // .prop -> .zip into a temp file -> extract `filename` -> MD5 must equal the attachment's md5
    // -> move to destDir/<key>/<filename> with the file's mtime. A web snapshot's zip is unpacked
    // whole (installSnapshot). On failure nothing is left behind.
    bool download(const AttachmentRecord&, const std::string& destDir, UserError*) override;
    // Free bytes at a path (statvfs by default); tests inject their own.
    void setFreeSpaceProbe(std::function<uint64_t(const std::string& path)>);

private:
    HttpRequest get(const std::string& name) const;
    UserError fail(const HttpResponse&) const;

    Http& http_;
    std::string base_, user_, password_, host_;
    std::function<uint64_t(const std::string&)> freeSpace_;
    std::function<void(int64_t, int64_t)> progress_;
};

// The name the downloaded file gets inside <destDir>/<key>/: the server's filename reduced to
// its last component; KEY.<ext> when that is empty, "." or "..".
std::string localFileName(const AttachmentRecord&);

// Hex MD5 of a file's contents; empty if it can't be read.
std::string md5OfFile(const std::string& path);

}  // namespace zk
