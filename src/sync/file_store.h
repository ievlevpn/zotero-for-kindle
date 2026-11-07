#pragma once
// Where attachment files come from: the user's WebDAV server, or Zotero's own storage. The sync
// worker uses whichever the account is set up for (WebDAV when its address is set).

#include "net/errors.h"
#include "store/records.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace zk {

struct Config;
class Http;

class FileStore {
public:
    virtual ~FileStore() = default;
    // The file into destDir/<key>/<localFileName>, its MD5 checked against the attachment's.
    // On failure nothing is left behind.
    virtual bool download(const AttachmentRecord&, const std::string& destDir, UserError*) = 0;
    // Called while the file downloads (bytes done, total; total 0 = unknown).
    virtual void setProgress(std::function<void(int64_t done, int64_t total)>) = 0;
};

// WebDAV when the account has a WebDAV address, Zotero storage otherwise.
std::unique_ptr<FileStore> makeFileStore(Http&, const Config&);

}  // namespace zk
