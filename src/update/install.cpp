#include "update/install.h"

#include "core/log.h"

#include <fcntl.h>
#include <miniz.h>
#include <sqlite3.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace zk {

namespace fs = std::filesystem;

namespace {

const char* const kSwapped[] = {"bin", "lib", "share"};
const char* const kRequiredFiles[] = {"bin/zotero", "bin/zk-sync", "lib/libzk-epub.so", "lib/libzk-pdf.so",
                                      "share/cacert.pem", "Zotero.sh"};

UserError incomplete(const std::string& details) {
    return {ErrorKind::Checksum, "The update is incomplete, so nothing was installed.", details};
}

bool safeName(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos) return false;
    for (const auto& part : fs::path(name))
        if (part == "..") return false;
    return true;
}

// What's missing from an unpacked release ("" = complete).
std::string missingFrom(const fs::path& dir) {
    std::error_code ec;
    for (const char* f : kRequiredFiles)
        if (!fs::is_regular_file(dir / f, ec) || fs::file_size(dir / f, ec) == 0) return f;
    const fs::path hyph = dir / "share/crengine/hyph";
    if (!fs::is_directory(hyph, ec) || fs::is_empty(hyph, ec)) return "share/crengine/hyph";
    return "";
}

bool writeDurably(const fs::path& path, const std::string& text) {
    const fs::path tmp = path.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    std::error_code ec;
    if (ok) fs::rename(tmp, path, ec);
    if (const int dir = ::open(path.parent_path().c_str(), O_RDONLY); dir >= 0) {  // the rename, too
        ::fsync(dir);
        ::close(dir);
    }
    return ok && !ec;
}

std::string readLine(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

// A consistent copy of a live database (WAL included) through SQLite's backup API.
bool backupDatabase(const std::string& from, const std::string& to, const std::function<void(const char*)>& beat,
                    std::string* error) {
    sqlite3 *src = nullptr, *dst = nullptr;
    bool ok = sqlite3_open_v2(from.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
              sqlite3_open(to.c_str(), &dst) == SQLITE_OK;
    if (ok) {
        sqlite3_backup* b = sqlite3_backup_init(dst, "main", src, "main");
        ok = b != nullptr;
        int rc = SQLITE_OK;
        while (ok && (rc = sqlite3_backup_step(b, 256)) != SQLITE_DONE) {
            if (rc != SQLITE_OK && rc != SQLITE_BUSY && rc != SQLITE_LOCKED) ok = false;
            if (beat) beat("update: backing up the library");
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) sqlite3_sleep(50);
        }
        if (b && sqlite3_backup_finish(b) != SQLITE_OK) ok = false;
    }
    if (!ok && error) *error = std::string("database backup: ") + sqlite3_errmsg(dst ? dst : src);
    sqlite3_close(src);
    sqlite3_close(dst);
    return ok;
}

bool parseState(const std::string& line, std::string* word, Version* from, Version* to) {
    std::istringstream in(line);
    std::string f, t;
    if (!(in >> *word >> f >> t)) return false;
    auto vf = parseVersion(f), vt = parseVersion(t);
    if (!vf || !vt) return false;
    *from = *vf;
    *to = *vt;
    return true;
}

}  // namespace

bool unpackRelease(const std::string& zipPath, const std::string& newDir, UserError* err) {
    const fs::path out(newDir);
    std::error_code ec;
    fs::remove_all(out, ec);
    auto fail = [&](UserError e) {
        std::error_code ignored;
        fs::remove_all(out, ignored);
        log(LogLevel::Warn, "update: unpack failed: %s", e.details.c_str());
        if (err) *err = std::move(e);
        return false;
    };
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) return fail(incomplete("not a zip"));
    bool ok = true;
    std::string details;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && ok; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            ok = false;
            details = "unreadable entry";
            break;
        }
        const std::string name = st.m_filename;
        if (!safeName(name)) {
            ok = false;
            details = "unsafe entry " + name;
            break;
        }
        if (st.m_is_directory) continue;
        fs::path dest;
        if (name.rfind("zotero/", 0) == 0) dest = out / name.substr(7);
        else if (name == "documents/Zotero.sh") dest = out / "Zotero.sh";
        else if (name == "documents/Zotero-no-plugins.sh") dest = out / "Zotero-no-plugins.sh";  // optional
        else continue;
        fs::create_directories(dest.parent_path(), ec);
        if (ec || !mz_zip_reader_extract_to_file(&zip, i, dest.c_str(), 0)) {
            ok = false;
            details = "can't extract " + name;
        }
    }
    mz_zip_reader_end(&zip);
    if (!ok) return fail(incomplete(details));
    const std::string missing = missingFrom(out);
    if (!missing.empty()) return fail(incomplete("missing " + missing));
    const auto exec = fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec;
    for (const auto& f : fs::directory_iterator(out / "bin", ec)) fs::permissions(f.path(), exec, ec);
    fs::permissions(out / "Zotero.sh", exec, ec);
    if (fs::exists(out / "Zotero-no-plugins.sh", ec)) fs::permissions(out / "Zotero-no-plugins.sh", exec, ec);
    return true;
}

bool installRelease(const InstallPaths& p, const std::string& newDir, Version from, Version to,
                    std::function<void(const char*)> beat, UserError* err, InstallHooks hooks) {
    const fs::path app(p.appDir), update(p.updateDir()), old = update / "old", trash = update / "trash";
    const fs::path fresh(newDir);
    auto step = [&](const char* what) {
        if (beat) beat(what);
    };
    if (!hooks.flush) hooks.flush = [] { ::sync(); };
    std::error_code ec;
    if (fs::exists(update / "state")) {  // an earlier update the launcher hasn't settled: its backup stays
        log(LogLevel::Warn, "update: %s not installed: an earlier update is pending", to.str().c_str());
        if (err) *err = {ErrorKind::Other, "Restart Zotero before updating.", "update/state exists"};
        return false;
    }
    const std::string configBackup = p.configFile + ".old";
    auto failBeforeSwap = [&](const std::string& details) {
        std::error_code ignored;
        fs::remove_all(old, ignored);
        if (!p.configFile.empty()) fs::remove(configBackup, ignored);
        log(LogLevel::Warn, "update: install of %s failed before the swap: %s", to.str().c_str(), details.c_str());
        if (err) *err = {ErrorKind::Other, "Couldn't install the update. Nothing was changed.", details};
        return false;
    };
    if (p.appDir.empty()) return failBeforeSwap("no app folder");
    if (const std::string missing = missingFrom(fresh); !missing.empty()) return failBeforeSwap("missing " + missing);

    // 1. Copies of everything a rollback restores; the running install stays as it is.
    fs::remove_all(old, ec);
    fs::create_directories(old, ec);
    if (ec) return failBeforeSwap("can't create " + old.string() + ": " + ec.message());
    for (const char* d : kSwapped) {
        step("update: backing up the app");
        if (fs::exists(app / d)) fs::copy(app / d, old / d, fs::copy_options::recursive, ec);
        if (ec) return failBeforeSwap(std::string("backup of ") + d + ": " + ec.message());
    }
    if (fs::exists(p.launcherPath)) fs::copy_file(p.launcherPath, old / "Zotero.sh", ec);
    if (ec) return failBeforeSwap("backup of the launcher: " + ec.message());
    // The account's backup stays beside it, off the USB drive, private.
    if (!p.configFile.empty() && fs::exists(p.configFile)) {
        fs::copy_file(p.configFile, configBackup, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::permissions(configBackup, fs::perms::owner_read | fs::perms::owner_write, ec);
    }
    if (ec) return failBeforeSwap("backup of the account: " + ec.message());
    std::string dbError;
    if (fs::exists(p.dbPath) && !backupDatabase(p.dbPath, (old / "zotero.db").string(), beat, &dbError))
        return failBeforeSwap(dbError);

    // 2. From here on an interrupted install is undone by the launcher: the backup must be on disk first.
    hooks.flush();
    if (!writeDurably(update / "state", "installing " + from.str() + " " + to.str() + "\n"))
        return failBeforeSwap("can't write the state");

    // 3. The swap.
    step("update: swapping in the new version");
    auto failDuringSwap = [&](const std::string& details) {
        log(LogLevel::Error, "update: install of %s failed during the swap: %s", to.str().c_str(), details.c_str());
        if (err)
            *err = {ErrorKind::Other, "The update didn't finish. Zotero will go back to " + from.str() + ".", details};
        return false;
    };
    fs::remove_all(trash, ec);
    fs::create_directories(trash, ec);
    for (const char* d : kSwapped) {
        if (fs::exists(app / d)) fs::rename(app / d, trash / d, ec);
        if (!ec) fs::rename(fresh / d, app / d, ec);
        if (ec) return failDuringSwap(std::string("swap of ") + d + ": " + ec.message());
    }
    const fs::path launcherTmp = p.launcherPath + ".new";
    fs::copy_file(fresh / "Zotero.sh", launcherTmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::permissions(launcherTmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read, ec);
    if (!ec) fs::rename(launcherTmp, p.launcherPath, ec);
    if (ec) return failDuringSwap("launcher: " + ec.message());
    // The recovery launcher (safe mode) beside it: best effort, an older release has none, and a
    // failure here never fails the install (nothing rolls it back either).
    if (fs::exists(fresh / "Zotero-no-plugins.sh", ec)) {
        const fs::path second = fs::path(p.launcherPath).parent_path() / "Zotero-no-plugins.sh";
        const fs::path secondTmp = second.string() + ".new";
        std::error_code e2;
        fs::copy_file(fresh / "Zotero-no-plugins.sh", secondTmp, fs::copy_options::overwrite_existing, e2);
        if (!e2) fs::permissions(secondTmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read, e2);
        if (!e2) fs::rename(secondTmp, second, e2);
        if (e2) {
            log(LogLevel::Warn, "update: Zotero-no-plugins.sh not written: %s", e2.message().c_str());
            fs::remove(secondTmp, e2);
        }
    }
    if (fs::exists(fresh / "LICENSE")) fs::copy_file(fresh / "LICENSE", app / "LICENSE", fs::copy_options::overwrite_existing, ec);
    fs::remove_all(fresh, ec);

    // 4. Done: the launcher starts the new version next.
    hooks.flush();
    if (!writeDurably(update / "state", "installed " + from.str() + " " + to.str() + "\n"))
        return failDuringSwap("can't write the state");
    log(LogLevel::Info, "update: %s installed over %s; restarting", to.str().c_str(), from.str().c_str());
    return true;
}

Confirmed confirmStartup(const InstallPaths& p, Version running) {
    Confirmed c;
    if (p.appDir.empty()) return c;
    const fs::path update(p.updateDir());
    std::error_code ec;
    std::string word;
    if (fs::exists(update / "rolled-back", ec)) {
        std::string from, to;
        std::istringstream in(readLine(update / "rolled-back"));
        in >> from >> to;
        fs::remove(update / "rolled-back", ec);
        for (const char* leftover : {"new", "zotero-for-kindle.zip", "zotero-for-kindle.zip.part"})  // the failed release
            fs::remove_all(update / leftover, ec);
        if (auto f = parseVersion(from), t = parseVersion(to); f && t) {
            c = {StartupNote::RolledBack, *f, *t};
            log(LogLevel::Warn, "update: %s didn't start; back on %s", t->str().c_str(), f->str().c_str());
        }
        return c;
    }
    Version from, to;
    if (!fs::exists(update / "state", ec)) {  // a download that was never installed
        for (const char* leftover : {"new", "zotero-for-kindle.zip", "zotero-for-kindle.zip.part"})
            fs::remove_all(update / leftover, ec);
        return c;
    }
    if (!parseState(readLine(update / "state"), &word, &from, &to)) return c;
    if ((word != "installed" && word != "tried") || to != running) return c;
    for (const char* leftover : {"state", "old", "new", "trash", "zotero-for-kindle.zip", "zotero-for-kindle.zip.part"})
        fs::remove_all(update / leftover, ec);
    if (!p.configFile.empty()) fs::remove(p.configFile + ".old", ec);
    log(LogLevel::Info, "update: %s confirmed", to.str().c_str());
    return {StartupNote::Updated, from, to};
}

Confirmed confirmStartup(const InstallPaths& p) { return confirmStartup(p, appVersion()); }

}  // namespace zk
