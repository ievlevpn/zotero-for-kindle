#pragma once
// Zotero's WebDAV file storage: each attachment is <key>.zip (the file) plus <key>.prop (XML
// with mtime and hash) in the configured zotero/ folder. Downloads only, GET only.

#include "net/errors.h"
#include "net/http.h"
#include "store/records.h"

#include <cstdint>
#include <optional>
#include <string>

namespace zk {

struct PropInfo {
    int64_t mtime = 0;  // milliseconds since the epoch
    std::string hash;   // MD5 of the file, hex
};

class WebDav {
public:
    WebDav(Http&, std::string baseUrl /* ends with /zotero/ */, std::string user, std::string password);

    std::optional<PropInfo> prop(const std::string& key, UserError*);
    // .prop -> .zip into a temp file -> extract `filename` -> MD5 must equal the attachment's md5
    // -> move to destDir/<key>/<filename> with the file's mtime. On failure nothing is left behind.
    bool download(const AttachmentRecord&, const std::string& destDir, UserError*);

private:
    HttpRequest get(const std::string& name) const;
    UserError fail(const HttpResponse&) const;

    Http& http_;
    std::string base_, user_, password_, host_;
};

// Hex MD5 of a file's contents; empty if it can't be read.
std::string md5OfFile(const std::string& path);

}  // namespace zk
