#include "sync/webdav.h"

#include "core/log.h"
#include "sync/snapshot_zip.h"

extern "C" {
#include <md5.h>
}
#include <miniz.h>
#include <pugixml.hpp>

#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utime.h>

namespace zk {

namespace fs = std::filesystem;

namespace {

// Removes a path (file or directory) when it goes out of scope, unless released.
struct Cleanup {
    fs::path path;
    ~Cleanup() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

uint64_t statvfsFree(const std::string& path) {
    struct statvfs st {};
    if (::statvfs(path.c_str(), &st) != 0) return UINT64_MAX;  // unknown: don't block on it
    return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
}

UserError fileError(ErrorKind kind, std::string message, std::string details) {
    return UserError{kind, std::move(message), std::move(details)};
}

void setError(UserError* err, UserError e) {
    log(LogLevel::Warn, "webdav: %s (%s)", e.message.c_str(), e.details.c_str());
    if (err) *err = std::move(e);
}

}  // namespace

std::string localFileName(const AttachmentRecord& a) {
    // The filename comes from server data: never let it name a path outside <destDir>/<key>/.
    std::string name = a.filename;
    if (size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") {
        const std::string ext = a.contentType == "application/pdf"        ? ".pdf"
                                : a.contentType == "application/epub+zip" ? ".epub"
                                : isWebSnapshot(a.contentType)            ? ".html"
                                                                          : "";
        name = a.key + ext;
    }
    return name;
}

void WebDav::setFreeSpaceProbe(std::function<uint64_t(const std::string&)> f) { freeSpace_ = std::move(f); }

std::string md5OfFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    MD5_CTX ctx;
    MD5_Init(&ctx);
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) MD5_Update(&ctx, buf, static_cast<unsigned long>(n));
    const bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) return {};
    unsigned char out[16];
    MD5_Final(out, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", out[i]);
    return hex;
}

WebDav::WebDav(Http& http, std::string baseUrl, std::string user, std::string password)
    : http_(http), base_(std::move(baseUrl)), user_(std::move(user)), password_(std::move(password)), host_(hostOf(base_)),
      freeSpace_(statvfsFree) {
    if (base_.empty()) return;
    if (base_.back() != '/') base_ += '/';
    // Zotero desktop takes the server's address and adds zotero/ itself; accept that address too.
    const std::string folder = "/zotero/";
    if (base_.size() < folder.size() || base_.compare(base_.size() - folder.size(), folder.size(), folder) != 0)
        base_ += "zotero/";
}

HttpRequest WebDav::get(const std::string& name) const {
    HttpRequest req;
    req.url = base_ + name;
    req.user = user_;
    req.password = password_;
    if (isTailnetHost(host_)) req.connectTimeoutS = 5;  // so "Tailscale isn't running" comes quickly
    return req;
}

UserError WebDav::fail(const HttpResponse& r) const {
    return classify(r, r.curlCode ? probeNetState() : NetState{}, host_, isTailnetHost(host_), "GET");
}

bool WebDav::check(UserError* err) {
    HttpRequest req = get("");
    req.method = "PROPFIND";
    req.headers.emplace_back("Depth", "0");
    HttpResponse r = http_.send(req);
    if (r.curlCode == 0 && (r.status == 207 || r.status == 200)) return true;
    if (r.curlCode == 0 && r.status == 404) {
        setError(err, fileError(ErrorKind::NotOnServer, "There's no zotero folder at that WebDAV address.", "HTTP 404"));
        return false;
    }
    setError(err, fail(r));
    return false;
}

std::optional<PropInfo> WebDav::prop(const std::string& key, UserError* err) {
    HttpResponse r = http_.send(get(key + ".prop"));
    if (r.curlCode || r.status != 200) return setError(err, fail(r)), std::nullopt;
    pugi::xml_document doc;
    if (!doc.load_buffer(r.body.data(), r.body.size()))
        return setError(err, fileError(ErrorKind::Other, "The server's file info for " + key + " is unreadable.", "bad .prop XML")),
               std::nullopt;
    pugi::xml_node props = doc.child("properties");
    PropInfo info;
    info.mtime = props.child("mtime").text().as_llong(0);
    info.hash = props.child("hash").text().as_string();
    return info;
}

bool WebDav::download(const AttachmentRecord& a, const std::string& destDir, UserError* err) {
    auto info = prop(a.key, err);
    if (!info) return false;

    std::error_code ec;
    fs::create_directories(destDir, ec);
    const fs::path zipPath = fs::path(destDir) / ("." + a.key + ".zip.part");
    const fs::path stage = fs::path(destDir) / ("." + a.key + ".part");
    Cleanup zipCleanup{zipPath}, stageCleanup{stage};

    HttpRequest req = get(a.key + ".zip");
    req.outFile = zipPath.string();
    req.timeoutS = 600;
    req.onProgress = progress_;
    const std::string name = localFileName(a);
    auto noSpace = [&](const std::string& details) {
        return fileError(ErrorKind::DiskFull, "Not enough space for " + name + ".", details);
    };
    HttpResponse r = http_.send(req);
    if (r.curlCode == 23 /* CURLE_WRITE_ERROR */ && freeSpace_(destDir) < (1u << 20))
        return setError(err, noSpace("curl 23: " + r.curlError)), false;
    if (r.curlCode || r.status != 200) return setError(err, fail(r)), false;
    if (isWebSnapshot(a.contentType))  // the page and its files, all of them
        return installSnapshot(a, zipPath.string(), destDir, info->mtime ? info->mtime : a.mtime,
                               [&] { return freeSpace_(destDir); }, err);

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0))
        return setError(err, fileError(ErrorKind::Other, "The server's copy of " + a.filename + " is damaged.",
                                       std::string("zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)))),
               false;
    int index = mz_zip_reader_locate_file(&zip, a.filename.c_str(), nullptr, 0);
    if (index < 0 && mz_zip_reader_get_num_files(&zip) == 1) index = 0;  // name stored differently
    if (index >= 0) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &st) && st.m_uncomp_size > freeSpace_(destDir)) {
            mz_zip_reader_end(&zip);
            return setError(err, noSpace(std::to_string(st.m_uncomp_size) + " bytes needed")), false;
        }
    }
    fs::create_directories(stage, ec);
    const fs::path staged = stage / name;
    const bool extracted = index >= 0 && mz_zip_reader_extract_to_file(&zip, static_cast<mz_uint>(index), staged.c_str(), 0);
    const bool outOfSpace = !extracted && index >= 0 && freeSpace_(destDir) < (1u << 20);
    const std::string zipErr = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    mz_zip_reader_end(&zip);
    if (outOfSpace) return setError(err, noSpace("zip: " + zipErr)), false;
    if (!extracted)
        return setError(err, fileError(index < 0 ? ErrorKind::NotOnServer : ErrorKind::Other,
                                       index < 0 ? a.filename + " isn't in the server's copy." : "Can't unpack " + a.filename + ".",
                                       "zip: " + zipErr)),
               false;

    const std::string md5 = md5OfFile(staged.string());
    if (md5.empty() || md5 != a.md5)
        return setError(err, fileError(ErrorKind::Checksum,
                                       name + " doesn't match Zotero's copy (checksum). Sync it again from your computer.",
                                       "md5 " + md5 + " != " + a.md5)),
               false;

    const int64_t mtimeMs = info->mtime ? info->mtime : a.mtime;
    if (mtimeMs > 0) {
        struct utimbuf t {};
        t.actime = t.modtime = static_cast<time_t>(mtimeMs / 1000);
        ::utime(staged.c_str(), &t);
    }
    const fs::path finalDir = fs::path(destDir) / a.key;
    fs::create_directories(finalDir, ec);
    fs::rename(staged, finalDir / name, ec);
    if (ec) return setError(err, fileError(ErrorKind::Other, "Can't save " + name + ": " + ec.message() + ".", ec.message())), false;
    return true;
}

}  // namespace zk
