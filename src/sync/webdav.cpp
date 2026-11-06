#include "sync/webdav.h"

#include "core/log.h"

extern "C" {
#include <md5.h>
}
#include <miniz.h>
#include <pugixml.hpp>

#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
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

UserError fileError(ErrorKind kind, std::string message, std::string details) {
    return UserError{kind, std::move(message), std::move(details)};
}

void setError(UserError* err, UserError e) {
    log(LogLevel::Warn, "webdav: %s (%s)", e.message.c_str(), e.details.c_str());
    if (err) *err = std::move(e);
}

}  // namespace

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
    : http_(http), base_(std::move(baseUrl)), user_(std::move(user)), password_(std::move(password)), host_(hostOf(base_)) {
    if (!base_.empty() && base_.back() != '/') base_ += '/';
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
    HttpResponse r = http_.send(req);
    if (r.curlCode || r.status != 200) return setError(err, fail(r)), false;

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0))
        return setError(err, fileError(ErrorKind::Other, "The server's copy of " + a.filename + " is damaged.",
                                       std::string("zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)))),
               false;
    int index = mz_zip_reader_locate_file(&zip, a.filename.c_str(), nullptr, 0);
    if (index < 0 && mz_zip_reader_get_num_files(&zip) == 1) index = 0;  // name stored differently
    fs::create_directories(stage, ec);
    const fs::path staged = stage / a.filename;
    const bool extracted = index >= 0 && mz_zip_reader_extract_to_file(&zip, static_cast<mz_uint>(index), staged.c_str(), 0);
    const std::string zipErr = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    mz_zip_reader_end(&zip);
    if (!extracted)
        return setError(err, fileError(index < 0 ? ErrorKind::NotOnServer : ErrorKind::Other,
                                       index < 0 ? a.filename + " isn't in the server's copy." : "Can't unpack " + a.filename + ".",
                                       "zip: " + zipErr)),
               false;

    const std::string md5 = md5OfFile(staged.string());
    if (md5.empty() || md5 != a.md5)
        return setError(err, fileError(ErrorKind::Checksum,
                                       a.filename + " doesn't match Zotero's copy (checksum). Sync it again from your computer.",
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
    fs::rename(staged, finalDir / a.filename, ec);
    if (ec)
        return setError(err, fileError(ErrorKind::DiskFull, "Can't save " + a.filename + ": " + ec.message() + ".", ec.message())),
               false;
    return true;
}

}  // namespace zk
