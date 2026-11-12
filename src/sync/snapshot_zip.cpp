#include "sync/snapshot_zip.h"

#include "core/log.h"
#include "sync/webdav.h"  // localFileName, md5OfFile

#include <miniz.h>

#include <sys/stat.h>
#include <utime.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace zk {

namespace fs = std::filesystem;

namespace {

// A relative path that stays inside the folder: no leading "/", no "\", no NUL, and no part that
// is empty, "." or ".." (nor one FAT folds to nothing: "...", " "). A directory's trailing "/" is allowed.
bool safeEntryName(std::string name, bool isDirectory) {
    if (isDirectory && !name.empty() && name.back() == '/') name.pop_back();
    if (name.empty() || name.find('\\') != std::string::npos || name.find('\0') != std::string::npos) return false;
    size_t from = 0;
    while (true) {
        const size_t slash = name.find('/', from);
        const std::string part = name.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
        if (part.find_last_not_of(". ") == std::string::npos) return false;  // "", ".", "..", "...", " ", also a leading "/"
        if (slash == std::string::npos) return true;
        from = slash + 1;
    }
}

bool isSymlink(const mz_zip_archive_file_stat& st) {
    const uint32_t mode = st.m_external_attr >> 16;
    return (mode & S_IFMT) == S_IFLNK;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

struct Cleanup {
    fs::path path;
    ~Cleanup() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

bool startsAsZip(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char sig[4] = {0};
    in.read(sig, 4);
    return in.gcount() == 4 && sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4;
}

bool installSnapshot(const AttachmentRecord& a, const std::string& zipPath, const std::string& destDir, int64_t mtimeMs,
                     const std::function<uint64_t()>& freeBytes, UserError* err, const SnapshotLimits& limits) {
    const std::string name = localFileName(a);
    auto fail = [&](ErrorKind kind, std::string message, std::string details) {
        log(LogLevel::Warn, "snapshot %s: %s (%s)", a.key.c_str(), message.c_str(), details.c_str());
        if (err) *err = UserError{kind, std::move(message), std::move(details)};
        return false;
    };
    auto damaged = [&](const std::string& details) {
        return fail(ErrorKind::Other, "The server's copy of " + name + " is damaged.", details);
    };

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0))
        return damaged(std::string("zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    struct Closer {
        mz_zip_archive* z;
        ~Closer() { mz_zip_reader_end(z); }
    } closer{&zip};
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    if (n > limits.maxEntries) return damaged(std::to_string(n) + " entries");
    std::vector<mz_uint> files;
    uint64_t declared = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return damaged("unreadable entry");
        const std::string entry = st.m_filename;
        if (!safeEntryName(entry, st.m_is_directory)) return damaged("unsafe entry " + entry);
        if (isSymlink(st)) return damaged("symlink " + entry);
        if (!st.m_is_supported || st.m_is_encrypted) return damaged("unsupported entry " + entry);
        if (st.m_is_directory) continue;
        declared += st.m_uncomp_size;
        if (declared > limits.maxBytes) return damaged("over " + std::to_string(limits.maxBytes >> 20) + " MB unpacked");
        files.push_back(i);
    }
    if (declared + (1u << 20) > freeBytes())
        return fail(ErrorKind::DiskFull, "Not enough space for " + name + ".", std::to_string(declared) + " bytes needed");

    std::error_code ec;
    const fs::path stage = fs::path(destDir) / ("." + a.key + ".part");  // swept at start if left behind
    fs::remove_all(stage, ec);
    Cleanup stageCleanup{stage};
    fs::create_directories(stage, ec);
    if (ec) return fail(ErrorKind::Other, "Can't save " + name + ": " + ec.message() + ".", ec.message());
    for (mz_uint i : files) {
        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&zip, i, &st);
        const fs::path dest = stage / st.m_filename;
        fs::create_directories(dest.parent_path(), ec);
        // miniz stops an entry that inflates past its declared size, so the cap holds.
        if (ec || !mz_zip_reader_extract_to_file(&zip, i, dest.c_str(), 0)) {
            if (freeBytes() < (1u << 20))
                return fail(ErrorKind::DiskFull, "Not enough space for " + name + ".", std::string("can't extract ") + st.m_filename);
            return damaged(std::string("can't extract ") + st.m_filename);
        }
    }

    // The page: under its name, else the one page at the top (a name stored another way).
    fs::path page = stage / name;
    if (!fs::is_regular_file(page, ec)) {
        std::vector<fs::path> pages;
        for (const auto& e : fs::directory_iterator(stage, ec)) {
            const std::string f = e.path().filename().string();
            if (e.is_regular_file(ec) && (endsWith(f, ".html") || endsWith(f, ".htm") || endsWith(f, ".HTML") || endsWith(f, ".HTM")))
                pages.push_back(e.path());
        }
        if (pages.size() != 1) return fail(ErrorKind::NotOnServer, name + " isn't in the server's copy.", "no " + name + " in the zip");
        fs::rename(pages[0], page, ec);
        if (ec) return fail(ErrorKind::Other, "Can't save " + name + ": " + ec.message() + ".", ec.message());
    }
    const std::string md5 = md5OfFile(page.string());
    if (md5.empty() || md5 != a.md5)
        return fail(ErrorKind::Checksum, name + " doesn't match Zotero's copy (checksum). Sync it again from your computer.",
                    "md5 " + md5 + " != " + a.md5);
    if (mtimeMs > 0) {
        struct utimbuf t {};
        t.actime = t.modtime = static_cast<time_t>(mtimeMs / 1000);
        ::utime(page.c_str(), &t);
    }
    const fs::path finalDir = fs::path(destDir) / a.key;
    fs::remove_all(finalDir, ec);  // an older copy and its files
    fs::rename(stage, finalDir, ec);
    if (ec) return fail(ErrorKind::Other, "Can't save " + name + ": " + ec.message() + ".", ec.message());
    stageCleanup.path.clear();
    return true;
}

}  // namespace zk
