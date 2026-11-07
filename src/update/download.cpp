#include "update/download.h"

#include "core/log.h"
#include "update/manifest.h"
#include "update/signature.h"

#include <sys/statvfs.h>

#include <filesystem>

namespace zk {

namespace fs = std::filesystem;

namespace {

int64_t statvfsFreeBytes(const std::string& dir) {
    struct statvfs st {};
    if (::statvfs(dir.c_str(), &st) != 0) return INT64_MAX;  // unknown: don't block on it
    return static_cast<int64_t>(st.f_bavail) * static_cast<int64_t>(st.f_frsize);
}

UserError refused(const std::string& details) {
    return {ErrorKind::Checksum, "The download didn't check out, so nothing was installed.", details};
}

}  // namespace

bool downloadRelease(Http& http, const ReleaseInfo& release, const std::array<uint8_t, 32>& publicKey,
                     const std::string& workDir, std::function<void(int64_t, int64_t)> progress,
                     std::shared_ptr<std::atomic<bool>> cancel, std::string* zipPath, UserError* err,
                     DownloadHooks hooks) {
    if (!hooks.freeSpace) hooks.freeSpace = statvfsFreeBytes;
    if (!hooks.probe) hooks.probe = probeNetState;
    const fs::path zip = fs::path(workDir) / "zotero-for-kindle.zip";
    const fs::path part = fs::path(workDir) / "zotero-for-kindle.zip.part";
    auto fail = [&](UserError e) {
        std::error_code ec;
        fs::remove(part, ec);
        fs::remove(zip, ec);
        log(LogLevel::Warn, "update: download of %s failed: %s", release.version.str().c_str(), e.details.c_str());
        if (err) *err = std::move(e);
        return false;
    };
    std::error_code ec;
    fs::create_directories(workDir, ec);
    const int64_t needed = release.zipSize * 4 + hooks.extraBytes + (int64_t{64} << 20);
    if (hooks.freeSpace(workDir) < needed)
        return fail({ErrorKind::DiskFull, "Not enough space on the Kindle for the update.",
                     "needs " + std::to_string(needed) + " bytes"});

    auto get = [&](const std::string& url, HttpResponse* out) {
        HttpRequest req;
        req.url = url;
        req.headers = {{"User-Agent", "zotero-for-kindle/" + appVersion().str()}};
        req.timeoutS = 60;
        req.cancel = cancel;
        *out = http.send(req);
        return out->curlCode == 0 && out->status == 200;
    };
    auto transportError = [&](const HttpResponse& r, const std::string& url) -> UserError {
        if (r.curlCode == 42 && cancel && *cancel) return {ErrorKind::Cancelled, "Cancelled.", "cancelled"};
        return classify(r, r.curlCode ? hooks.probe() : NetState{}, hostOf(url), false);
    };

    HttpResponse manifestResp, sigResp;
    if (!get(release.manifestUrl, &manifestResp)) return fail(transportError(manifestResp, release.manifestUrl));
    if (!get(release.sigUrl, &sigResp)) return fail(transportError(sigResp, release.sigUrl));
    if (!verifySignature(manifestResp.body, sigResp.body, publicKey)) return fail(refused("bad signature"));
    const auto manifest = parseManifest(manifestResp.body);
    if (!manifest) return fail(refused("unreadable manifest"));
    if (manifest->version != release.version)
        return fail(refused("manifest is for " + manifest->version.str() + ", release is " + release.version.str()));

    HttpRequest req;
    req.url = release.zipUrl;
    req.headers = {{"User-Agent", "zotero-for-kindle/" + appVersion().str()}};
    req.outFile = part.string();
    req.timeoutS = 1800;
    req.onProgress = progress;
    req.cancel = cancel;
    const HttpResponse r = http.send(req);
    if (r.curlCode != 0 || r.status != 200) return fail(transportError(r, release.zipUrl));
    const auto size = fs::file_size(part, ec);
    if (ec || static_cast<int64_t>(size) != manifest->size)
        return fail(refused("size " + std::to_string(ec ? 0 : size) + ", expected " + std::to_string(manifest->size)));
    if (sha512FileHex(part.string()) != manifest->sha512) return fail(refused("sha512 mismatch"));
    fs::rename(part, zip, ec);
    if (ec) return fail({ErrorKind::Other, "Couldn't save the update.", ec.message()});
    if (zipPath) *zipPath = zip.string();
    log(LogLevel::Info, "update: %s downloaded and verified", release.version.str().c_str());
    return true;
}

}  // namespace zk
