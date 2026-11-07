#include "sync/zotero_storage.h"

#include "sync/config.h"
#include "sync/webdav.h"  // localFileName, md5OfFile

#include <sys/statvfs.h>
#include <utime.h>

#include <filesystem>

namespace zk {

namespace fs = std::filesystem;

namespace {

uint64_t statvfsFreeBytes(const std::string& path) {
    struct statvfs st {};
    if (::statvfs(path.c_str(), &st) != 0) return UINT64_MAX;  // unknown: don't block on it
    return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
}

bool fail(UserError* err, UserError e) {
    if (err) *err = std::move(e);
    return false;
}

}  // namespace

ZoteroStorage::ZoteroStorage(Http& http, int64_t userId, std::string apiKey)
    : http_(http), userId_(userId), apiKey_(std::move(apiKey)), freeSpace_(statvfsFreeBytes) {}

bool ZoteroStorage::download(const AttachmentRecord& a, const std::string& destDir, UserError* err) {
    const std::string name = localFileName(a);
    // 1. Where the file is: Zotero answers with a redirect (the key stays with Zotero).
    HttpRequest where;
    where.url = "https://api.zotero.org/users/" + std::to_string(userId_) + "/items/" + a.key + "/file";
    where.headers = {{"Zotero-API-Key", apiKey_}, {"Zotero-API-Version", "3"}};
    where.followRedirects = false;
    where.timeoutS = 60;
    const HttpResponse r = http_.send(where);
    if (r.curlCode == 0 && r.status == 404)
        return fail(err, {ErrorKind::NotOnServer, name + " isn't in Zotero storage. Sync it from your computer first.",
                          "HTTP 404"});
    if (r.curlCode || r.status < 300 || r.status >= 400) {
        UserError e = classify(r, r.curlCode ? probeNetState() : NetState{}, "api.zotero.org", false);
        if (e.kind == ErrorKind::None) e = {ErrorKind::Other, "Zotero didn't send " + name + ".", "HTTP " + std::to_string(r.status)};
        return fail(err, e);
    }
    const auto location = r.headers.find("location");
    if (location == r.headers.end() || location->second.empty())
        return fail(err, {ErrorKind::Other, "Zotero didn't send " + name + ".", "redirect without a location"});

    // 2. The file itself, staged, then checked and moved into place.
    std::error_code ec;
    fs::create_directories(destDir, ec);
    const fs::path stage = fs::path(destDir) / ("." + a.key + ".part");
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{stage};
    fs::create_directories(stage, ec);
    const fs::path staged = stage / name;
    HttpRequest get;
    get.url = location->second;
    get.outFile = staged.string();
    get.timeoutS = 600;
    get.onProgress = progress_;
    const HttpResponse f = http_.send(get);
    if (f.curlCode == 23 /* CURLE_WRITE_ERROR */ && freeSpace_(destDir) < (1u << 20))
        return fail(err, {ErrorKind::DiskFull, "Not enough space for " + name + ".", "curl 23: " + f.curlError});
    if (f.curlCode || f.status != 200)
        return fail(err, classify(f, f.curlCode ? probeNetState() : NetState{}, hostOf(get.url), false));
    if (progress_) {
        const auto size = static_cast<int64_t>(fs::file_size(staged, ec));
        progress_(size, size);
    }
    const std::string md5 = md5OfFile(staged.string());
    if (md5.empty() || md5 != a.md5)
        return fail(err, {ErrorKind::Checksum,
                          name + " doesn't match Zotero's copy (checksum). Sync it again from your computer.",
                          "md5 " + md5 + " != " + a.md5});
    if (a.mtime > 0) {
        struct utimbuf t {};
        t.actime = t.modtime = static_cast<time_t>(a.mtime / 1000);
        ::utime(staged.c_str(), &t);
    }
    const fs::path finalDir = fs::path(destDir) / a.key;
    fs::create_directories(finalDir, ec);
    fs::rename(staged, finalDir / name, ec);
    if (ec) return fail(err, {ErrorKind::Other, "Can't save " + name + ": " + ec.message() + ".", ec.message()});
    return true;
}

std::unique_ptr<FileStore> makeFileStore(Http& http, const Config& c) {
    if (!c.webdavUrl.empty()) return std::make_unique<WebDav>(http, c.webdavUrl, c.webdavUser, c.webdavPassword);
    return std::make_unique<ZoteroStorage>(http, c.userId, c.apiKey);
}

}  // namespace zk
