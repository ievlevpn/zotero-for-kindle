#include "catalog/install.h"

#include "core/log.h"
#include "plugins/state.h"
#include "update/signature.h"

#include <miniz.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace zk::catalog {

namespace fs = std::filesystem;

namespace {

const char* const kDamaged = "The download is damaged.";
constexpr int64_t kSpareBytes = int64_t{16} << 20;  // left free after the download, and after the unpack

UserError damaged(const std::string& details) { return {ErrorKind::Checksum, kDamaged, details}; }

fs::path incomingDir(const std::string& pluginsDir) { return fs::path(pluginsDir) / ".incoming"; }
fs::path previousDir(const std::string& pluginsDir) { return fs::path(pluginsDir) / ".previous"; }

int64_t statvfsFreeBytes(const std::string& dir) {
    struct statvfs st {};
    if (::statvfs(dir.c_str(), &st) != 0) return INT64_MAX;  // unknown: don't block on it
    return static_cast<int64_t>(st.f_bavail) * static_cast<int64_t>(st.f_frsize);
}

std::optional<std::string> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// https on GitHub's own hosts only: anything else is refused before it is fetched.
bool allowedUrl(const std::string& url) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    const size_t end = url.find_first_of("/?#", scheme.size());
    const std::string host = lower(url.substr(scheme.size(), end == std::string::npos ? std::string::npos
                                                                                       : end - scheme.size()));
    if (host.find_first_of("@:") != std::string::npos) return false;  // no user info, no port
    if (host == "github.com" || host == "api.github.com" || host == "objects.githubusercontent.com") return true;
    const std::string suffix = ".githubusercontent.com";
    return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A path part as FAT stores it: trailing dots and spaces dropped, case folded.
std::string fatFolded(const std::string& part) {
    const size_t end = part.find_last_not_of(". ");
    return lower(end == std::string::npos ? "" : part.substr(0, end + 1));
}

// A relative path that stays inside the folder, with no "." or empty part (so "./data/x" can't pass
// for something else), and no part FAT would fold to nothing ("..", "...", " "). A directory entry's
// one trailing "/" is allowed.
bool safeName(std::string name, bool isDirectory) {
    if (isDirectory && !name.empty() && name.back() == '/') name.pop_back();
    if (name.empty() || name.find('\\') != std::string::npos || name.find('\0') != std::string::npos) return false;
    size_t from = 0;
    while (true) {
        const size_t slash = name.find('/', from);
        const std::string part = name.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
        if (part.empty() || fatFolded(part).empty()) return false;  // also a leading "/"
        if (slash == std::string::npos) return true;
        from = slash + 1;
    }
}

// What the plugin saved, kept in <id>/ across update and roll back: data/, its storage (zk.storage)
// and its log. A zip's own copy is never unpacked: "data", "DATA", "data." and "data " all land in
// data/ on FAT.
const char* const kUserItems[] = {"data", "storage.db", "storage.db-wal", "storage.db-shm", "storage.db-journal",
                                  "plugin.log", "plugin.log.1"};
bool isDataEntry(const std::string& name) {
    const std::string first = fatFolded(name.substr(0, name.find('/')));
    for (const char* item : kUserItems)
        if (first == item) return true;
    return false;
}

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}

bool isSymlink(const mz_zip_archive_file_stat& st) {
    const uint32_t mode = st.m_external_attr >> 16;
    return (mode & S_IFMT) == S_IFLNK;
}

// The zip into dir (which must not exist): every entry checked before anything is written.
// The zip into dir: every entry checked before anything is written; `freeBytes` must cover the tree.
bool unpack(const fs::path& zipPath, const fs::path& dir, const std::function<int64_t()>& freeBytes, UserError* err) {
    std::string details;
    const auto why = &details;
    auto bad = [&] {
        *err = damaged(details);
        return false;
    };
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) return *why = "not a zip", bad();
    struct Closer {
        mz_zip_archive* z;
        ~Closer() { mz_zip_reader_end(z); }
    } closer{&zip};
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    if (n > kMaxZipEntries) return *why = std::to_string(n) + " entries", bad();
    std::vector<mz_uint> files;
    uint64_t declared = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return *why = "unreadable entry", bad();
        const std::string name = st.m_filename;
        if (!safeName(name, st.m_is_directory)) return *why = "unsafe entry " + name, bad();
        if (isSymlink(st)) return *why = "symlink " + name, bad();
        if (!st.m_is_supported || st.m_is_encrypted) return *why = "unsupported entry " + name, bad();
        if (isDataEntry(name) || st.m_is_directory) continue;
        declared += st.m_uncomp_size;
        if (declared > static_cast<uint64_t>(kMaxTreeBytes)) return *why = "tree over 32 MB", bad();
        files.push_back(i);
    }
    if (freeBytes() < static_cast<int64_t>(declared) + kSpareBytes) {
        *err = {ErrorKind::DiskFull, "Not enough space.", "tree of " + std::to_string(declared) + " bytes"};
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return *why = "can't create " + dir.string(), bad();
    for (mz_uint i : files) {
        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&zip, i, &st);
        const fs::path dest = dir / st.m_filename;
        fs::create_directories(dest.parent_path(), ec);
        // miniz stops a file that inflates past its declared size, so the 32 MB holds.
        if (ec || !mz_zip_reader_extract_to_file(&zip, i, dest.c_str(), 0))
            return *why = std::string("can't extract ") + st.m_filename, bad();
    }
    return true;
}

std::optional<PluginManifest> manifestIn(const fs::path& dir) {
    auto text = readFile(dir / "plugin.json");
    if (!text) return std::nullopt;
    return parseManifest(*text, nullptr);
}

std::vector<Permission> addedPermissions(const PluginManifest& now, const std::optional<PluginManifest>& before) {
    std::vector<Permission> out;
    for (Permission p : now.permissions) {
        if (!before || !before->permissions.count(p)) {
            out.push_back(p);
        } else if (p == Permission::Net) {
            const auto& old = before->hosts;
            if (std::any_of(now.hosts.begin(), now.hosts.end(),
                            [&](const std::string& h) { return std::find(old.begin(), old.end(), h) == old.end(); }))
                out.push_back(p);
        }
    }
    return out;
}

bool setProvenance(const std::string& pluginsDir, const std::string& id, bool fromCatalog) {
    Cache c = loadCache(pluginsDir);
    if (fromCatalog == static_cast<bool>(c.fromCatalog.count(id))) return true;
    if (fromCatalog) c.fromCatalog.insert(id);
    else c.fromCatalog.erase(id);
    return saveCache(pluginsDir, c);
}

bool fail(UserError* err, const std::string& message, const std::string& details) {
    log(LogLevel::Warn, "catalog: %s (%s)", message.c_str(), details.c_str());
    if (err) *err = {ErrorKind::Other, message, details};
    return false;
}

// A unique place in .incoming/ for something on its way out; cleared at the next start at the latest.
fs::path trashPath(const std::string& pluginsDir, const std::string& what) {
    static std::atomic<unsigned> n{0};
    return incomingDir(pluginsDir) / ("." + what + "-" + std::to_string(++n));
}

// Moves `p` aside (one rename), then deletes it: a kill never leaves it half deleted in place.
void trash(const std::string& pluginsDir, const fs::path& p, std::error_code& ec) {
    if (!fs::exists(fs::symlink_status(p, ec))) {
        ec.clear();
        return;
    }
    const fs::path aside = trashPath(pluginsDir, "trash");
    fs::create_directories(aside.parent_path(), ec);
    if (!ec) fs::rename(p, aside, ec);
    if (ec) return;
    std::error_code ignored;
    fs::remove_all(aside, ignored);
}

void removeIncomingIfEmpty(const std::string& pluginsDir) {
    std::error_code ec;
    if (fs::is_empty(incomingDir(pluginsDir), ec)) fs::remove(incomingDir(pluginsDir), ec);
}

// The swap, steps 2–4 of install.h: `fresh` (code only, in .incoming/) becomes <id>; the current
// <id> becomes .previous/<id>; data/ follows to the new <id>. The old .previous/<id> is already gone.
bool swapIn(const std::string& pluginsDir, const std::string& id, const fs::path& fresh, std::string* why) {
    const fs::path cur = fs::path(pluginsDir) / id, prev = previousDir(pluginsDir) / id;
    std::error_code ec;
    const bool hadCurrent = fs::exists(fs::symlink_status(cur, ec));
    {  // the marker says a swap is under way: only then may repair put .previous/<id> back
        fs::create_directories(incomingDir(pluginsDir), ec);
        std::ofstream marker(swapMarker(pluginsDir, id), std::ios::trunc);
        marker << id;
        marker.flush();
        if (!marker) return *why = "swap marker", false;
    }
    if (hadCurrent) {
        fs::create_directories(prev.parent_path(), ec);
        if (!ec) fs::rename(cur, prev, ec);
        if (ec) return *why = "move aside: " + ec.message(), false;
    }
    fs::rename(fresh, cur, ec);
    if (ec) {
        *why = "move in: " + ec.message();
        std::error_code back;
        if (hadCurrent) fs::rename(prev, cur, back);  // as it was (or repairInterrupted does it)
        return false;
    }
    if (hadCurrent)
        for (const char* item : kUserItems)
            if (present(prev / item)) {
                fs::rename(prev / item, cur / item, ec);
                if (ec) return *why = std::string(item) + ": " + ec.message(), false;  // repairInterrupted moves it at the next start
            }
    fs::remove(swapMarker(pluginsDir, id), ec);
    return true;
}

// Before a swap: a data/ left in .previous/<id> goes back to <id>; if <id> has one too, nothing may
// go ahead (the swap would delete one of them).
bool dataSettled(const std::string& pluginsDir, const std::string& id, std::string* why) {
    repairOne(pluginsDir, id);
    for (const char* item : kUserItems) {
        const fs::path left = previousDir(pluginsDir) / id / item;
        if (std::string(item).rfind("plugin.log", 0) == 0 && present(left)) {  // only a log: <id>'s own wins
            std::error_code ec;
            fs::remove(left, ec);
        }
        if (present(left)) {
            *why = std::string(item) + " in both " + id + " and .previous/" + id;
            return false;
        }
    }
    return true;
}

bool validIdOr(const std::string& id, UserError* err) {
    if (validPluginId(id)) return true;
    return fail(err, "That isn't a plugin.", "bad id " + id);
}

}  // namespace

std::optional<Staged> stage(Http& http, const Entry& entry, const Latest& latest, const std::string& pluginsDir,
                            Version app, std::shared_ptr<std::atomic<bool>> cancel, UserError* err,
                            DownloadHooks hooks) {
    if (!hooks.freeSpace) hooks.freeSpace = statvfsFreeBytes;
    if (!hooks.probe) hooks.probe = probeNetState;
    const fs::path incoming = incomingDir(pluginsDir);
    const fs::path zip = incoming / (entry.id + ".zip"), part = incoming / (entry.id + ".zip.part");
    const fs::path tree = incoming / entry.id;
    auto refuse = [&](UserError e) -> std::optional<Staged> {
        std::error_code ec;
        fs::remove(zip, ec);
        fs::remove(part, ec);
        fs::remove_all(tree, ec);
        removeIncomingIfEmpty(pluginsDir);
        log(LogLevel::Warn, "catalog: %s %s not staged: %s", entry.id.c_str(), latest.version.str().c_str(),
            e.details.c_str());
        if (err) *err = std::move(e);
        return std::nullopt;
    };
    if (!validPluginId(entry.id)) return refuse(damaged("bad id"));
    if (!allowedUrl(latest.zipUrl) || !allowedUrl(latest.sigUrl))
        return refuse(damaged("not a GitHub URL: " + latest.zipUrl + " " + latest.sigUrl));
    if (latest.zipSize <= 0 || latest.zipSize > kMaxZipBytes)
        return refuse(damaged("zip size " + std::to_string(latest.zipSize)));

    std::error_code ec;
    fs::remove_all(tree, ec);  // an earlier staging of this id, not committed
    fs::create_directories(incoming, ec);
    if (ec) return refuse({ErrorKind::Other, "Couldn't save the download.", ec.message()});
    const int64_t needed = 3 * latest.zipSize + kSpareBytes;
    if (hooks.freeSpace(incoming.string()) < needed)
        return refuse({ErrorKind::DiskFull, "Not enough space.", "needs " + std::to_string(needed) + " bytes"});

    auto request = [&](const std::string& url, size_t maxBytes) {
        HttpRequest req;
        req.url = url;
        req.headers = {{"User-Agent", "zotero-for-kindle/" + appVersion().str()}};
        req.timeoutS = 300;
        req.maxBodyBytes = maxBytes;
        req.cancel = cancel;
        return req;
    };
    auto transportError = [&](const HttpResponse& r, const std::string& url) -> UserError {
        if (cancel && *cancel) return {ErrorKind::Cancelled, "Cancelled.", "cancelled"};
        if (r.tooLarge) return damaged("over the size limit: " + url);
        UserError e = classify(r, r.curlCode ? hooks.probe() : NetState{}, hostOf(url), false);
        if (e.kind == ErrorKind::NoWifi) e.message = "No Wi-Fi.";
        return e;
    };

    const HttpResponse sig = http.send(request(latest.sigUrl, 1024));
    if (sig.curlCode != 0 || sig.status != 200) return refuse(transportError(sig, latest.sigUrl));
    HttpRequest zipReq = request(latest.zipUrl, static_cast<size_t>(kMaxZipBytes));
    zipReq.outFile = part.string();
    const HttpResponse got = http.send(zipReq);
    if (got.curlCode != 0 || got.status != 200) return refuse(transportError(got, latest.zipUrl));
    const auto size = fs::file_size(part, ec);  // the size from the release is only a claim
    if (ec || size > static_cast<uintmax_t>(kMaxZipBytes)) return refuse(damaged("zip over 8 MB"));
    const auto bytes = readFile(part);
    if (!bytes) return refuse(damaged("can't read the zip"));
    if (!verifySignature(*bytes, sig.body, entry.publicKey))
        return refuse({ErrorKind::Checksum, "The download isn't signed by the plugin's author.", "bad signature"});
    fs::rename(part, zip, ec);
    if (ec) return refuse({ErrorKind::Other, "Couldn't save the download.", ec.message()});

    UserError unpackErr;
    if (!unpack(zip, tree, [&] { return hooks.freeSpace(incoming.string()); }, &unpackErr))
        return refuse(std::move(unpackErr));
    fs::remove(zip, ec);

    // The manifest, checked as the scanner will check it once installed.
    const auto scanned = scanPlugins(incoming.string(), app);
    const auto f = std::find_if(scanned.begin(), scanned.end(), [&](const PluginFolder& p) { return p.dir == entry.id; });
    if (f == scanned.end() || !f->manifest) return refuse(damaged(f == scanned.end() ? "no folder" : f->problem));
    const PluginManifest& m = *f->manifest;
    if (m.id != entry.id) return refuse({ErrorKind::Other, "It isn't the plugin the catalog lists.", "id " + m.id});
    if (m.version != latest.version.str())
        return refuse({ErrorKind::Other, "The release's version doesn't match its plugin.json.",
                       "plugin.json " + m.version + ", tag " + latest.version.str()});
    if (f->otherApi != 0) return refuse({ErrorKind::Other, f->problem, "api " + std::to_string(f->otherApi)});
    if (app < m.minApp) return refuse({ErrorKind::Other, "It needs app " + m.minApp.str() + ".", "minApp"});
    if (!f->problem.empty()) return refuse(damaged(f->problem));

    log(LogLevel::Info, "catalog: %s %s staged", entry.id.c_str(), m.version.c_str());
    return Staged{m, addedPermissions(m, manifestIn(fs::path(pluginsDir) / entry.id))};
}

bool commit(const std::string& pluginsDir, const std::string& id, UserError* err) {
    if (!validIdOr(id, err)) return false;
    const fs::path fresh = incomingDir(pluginsDir) / id;
    std::error_code ec;
    if (!fs::is_regular_file(fresh / "plugin.json", ec)) return fail(err, "Couldn't install the plugin.", "not staged");
    if (std::string why; !dataSettled(pluginsDir, id, &why)) return fail(err, "Couldn't install the plugin.", why);
    trash(pluginsDir, previousDir(pluginsDir) / id, ec);  // one kept: the older one goes
    if (ec) return fail(err, "Couldn't install the plugin.", "old previous: " + ec.message());
    std::string why;
    if (!swapIn(pluginsDir, id, fresh, &why)) return fail(err, "Couldn't install the plugin.", why);
    removeIncomingIfEmpty(pluginsDir);
    if (!setProvenance(pluginsDir, id, true)) log(LogLevel::Warn, "catalog: %s: provenance not saved", id.c_str());
    log(LogLevel::Info, "catalog: %s installed", id.c_str());
    return true;
}

bool rollBack(const std::string& pluginsDir, const std::string& id, UserError* err) {
    if (!validIdOr(id, err)) return false;
    const fs::path cur = fs::path(pluginsDir) / id, prev = previousDir(pluginsDir) / id;
    std::error_code ec;
    if (std::string why; !dataSettled(pluginsDir, id, &why)) return fail(err, "Couldn't roll back.", why);
    if (!fs::is_regular_file(prev / "plugin.json", ec) || !fs::is_directory(cur, ec))
        return fail(err, "There is no earlier version to go back to.", "nothing kept");
    // Step 1: the kept version out of the way (a kill here loses only it), then the same swap.
    const fs::path aside = trashPath(pluginsDir, "rollback");
    fs::create_directories(aside.parent_path(), ec);
    if (!ec) fs::rename(prev, aside, ec);
    if (ec) return fail(err, "Couldn't roll back.", "move aside: " + ec.message());
    std::string why;
    if (!swapIn(pluginsDir, id, aside, &why)) {
        std::error_code back;
        if (!fs::exists(prev, back)) fs::rename(aside, prev, back);
        return fail(err, "Couldn't roll back.", why);
    }
    removeIncomingIfEmpty(pluginsDir);
    log(LogLevel::Info, "catalog: %s rolled back", id.c_str());
    return true;
}

std::optional<std::string> previousVersion(const std::string& pluginsDir, const std::string& id) {
    if (!validPluginId(id)) return std::nullopt;
    auto m = manifestIn(previousDir(pluginsDir) / id);
    if (!m) return std::nullopt;
    return m->version;
}

bool removeFiles(const std::string& pluginsDir, const std::string& id, UserError* err) {
    if (!validIdOr(id, err)) return false;
    std::error_code ec;
    // The kept version first, so a kill midway never brings it back in place of the plugin.
    for (const fs::path& p : {previousDir(pluginsDir) / id, fs::path(pluginsDir) / id, incomingDir(pluginsDir) / id}) {
        trash(pluginsDir, p, ec);
        if (ec) return fail(err, "Couldn't remove the plugin.", p.string() + ": " + ec.message());
    }
    removeIncomingIfEmpty(pluginsDir);
    if (!setProvenance(pluginsDir, id, false)) return fail(err, "Couldn't remove the plugin.", "provenance");
    log(LogLevel::Info, "catalog: %s removed", id.c_str());
    return true;
}

std::string swapMarker(const std::string& pluginsDir, const std::string& id) {
    return (incomingDir(pluginsDir) / (".swap-" + id)).string();
}

std::vector<Permission> rollBackAdds(const std::string& pluginsDir, const std::string& id) {
    if (!validPluginId(id)) return {};
    auto kept = manifestIn(previousDir(pluginsDir) / id);
    if (!kept) return {};
    return addedPermissions(*kept, manifestIn(fs::path(pluginsDir) / id));
}

void repairOne(const std::string& pluginsDir, const std::string& id) {
    if (!validPluginId(id)) return;
    const fs::path cur = fs::path(pluginsDir) / id, prev = previousDir(pluginsDir) / id;
    std::error_code e;
    const bool swapping = present(swapMarker(pluginsDir, id));
    fs::remove(swapMarker(pluginsDir, id), e);
    if (!fs::exists(fs::symlink_status(prev, e))) return;
    if (!swapping && !present(cur)) {
        // No swap was under way: the user deleted the plugin (over USB). It stays deleted: its kept
        // version goes (code only; one holding what the plugin saved is left alone), and so does
        // what the app remembered of it, so nothing turns it on again.
        bool saved = false;
        for (const char* item : kUserItems) saved = saved || present(prev / item);
        if (!saved) trash(pluginsDir, prev, e);
        PluginState(pluginsDir + "/.state.json").forget(id);
        setProvenance(pluginsDir, id, false);
        log(LogLevel::Info, "catalog: %s was deleted by hand: its kept version and state are gone too", id.c_str());
        return;
    }
    if (!fs::exists(fs::symlink_status(cur, e))) {
        fs::rename(prev, cur, e);
        log(LogLevel::Warn, "catalog: %s: put the previous version back after an interrupted swap%s", id.c_str(),
            e ? (" — failed: " + e.message()).c_str() : "");
    } else {
        for (const char* item : kUserItems)
            if (present(prev / item) && !present(cur / item)) {
                fs::rename(prev / item, cur / item, e);
                log(LogLevel::Warn, "catalog: %s: moved %s in after an interrupted swap%s", id.c_str(), item,
                    e ? (" — failed: " + e.message()).c_str() : "");
            }
    }
}

void repairInterrupted(const std::string& pluginsDir) {
    std::error_code ec;
    std::vector<std::string> kept;
    for (fs::directory_iterator it(previousDir(pluginsDir), ec), end; !ec && it != end; it.increment(ec))
        kept.push_back(it->path().filename().string());
    for (const auto& id : kept) repairOne(pluginsDir, id);
    fs::remove_all(incomingDir(pluginsDir), ec);
    // A folder deleted by hand takes what the app remembered of it too: a copy put back is new
    // (off, its permissions shown first), as after Remove.
    PluginState state(pluginsDir + "/.state.json");
    for (const auto& id : state.ids())
        if (validPluginId(id) && !present(fs::path(pluginsDir) / id)) state.forget(id);
}

}  // namespace zk::catalog
