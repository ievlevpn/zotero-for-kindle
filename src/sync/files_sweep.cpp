#include "sync/files_sweep.h"

#include "core/log.h"

#include <filesystem>

namespace zk {

namespace fs = std::filesystem;

namespace {

bool looksLikeKey(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// ".<key>.part" or ".<key>.zip.part": the key, else "".
std::string partKey(const std::string& name) {
    if (name.size() < 2 || name[0] != '.') return {};
    for (const char* suffix : {".zip.part", ".part"}) {
        const std::string sfx(suffix);
        if (name.size() == 1 + 8 + sfx.size() && name.compare(9, sfx.size(), sfx) == 0) {
            const std::string key = name.substr(1, 8);
            if (looksLikeKey(key)) return key;
        }
    }
    return {};
}

}  // namespace

FilesSweep sweepFiles(const std::string& filesDir, const std::unordered_set<std::string>& attachmentKeys) {
    FilesSweep out;
    std::error_code ec;
    fs::directory_iterator it(filesDir, ec), end;
    if (ec) return out;
    std::vector<fs::directory_entry> entries;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        entries.push_back(*it);
    }
    for (const auto& e : entries) {
        const std::string name = e.path().filename().string();
        std::error_code sec;
        const fs::file_status st = e.symlink_status(sec);  // the entry itself, not where a link points
        if (sec || fs::is_symlink(st)) continue;
        bool remove = false;
        if (fs::is_directory(st) && looksLikeKey(name)) {
            remove = !attachmentKeys.count(name);  // its attachment is gone from the library
        } else if (!partKey(name).empty() && (fs::is_directory(st) || fs::is_regular_file(st))) {
            remove = true;  // an interrupted download
        }
        if (!remove) continue;
        std::error_code rec;
        fs::remove_all(e.path(), rec);  // doesn't follow links inside it
        if (rec) {
            log(LogLevel::Warn, "files: couldn't remove %s: %s", e.path().c_str(), rec.message().c_str());
        } else {
            log(LogLevel::Info, "files: removed %s (%s)", e.path().c_str(),
                fs::is_directory(st) && looksLikeKey(name) ? "not in the library" : "unfinished download");
            out.removed.push_back(name);
        }
    }
    return out;
}

}  // namespace zk
