#include "plugins/fulltext.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "docs/engine_factory.h"

namespace zk {

namespace fs = std::filesystem;

namespace {

bool fail(UserError* err, std::string message, std::string details) {
    if (err) *err = UserError{ErrorKind::Other, std::move(message), std::move(details)};
    return false;
}

// Keys and checksums are alphanumeric in practice; anything else would leave the cache folder.
bool safeName(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

std::optional<std::string> readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return ss.str();
}

// Drops the least recently used entries (by modification time: a hit touches its entry) other than
// `keep` until the cache holds at most kMaxFullTextCacheBytes.
void prune(const fs::path& dir, const fs::path& keep) {
    struct Entry {
        fs::file_time_type used;
        uint64_t size;
        fs::path path;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const uint64_t size = it->file_size(e2);
        if (e2) continue;
        total += size;
        if (it->path() != keep) entries.push_back({it->last_write_time(e2), size, it->path()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const Entry& e : entries) {
        if (total <= kMaxFullTextCacheBytes) break;
        std::error_code rc;
        if (fs::remove(e.path, rc)) total -= e.size;
    }
}

}  // namespace

namespace {

std::optional<std::string> extract(const AttachmentRecord& a, const std::string& pluginsDir, const EngineEnv& env,
                                   UserError* err, const EngineLoader& loader) {
    if (a.linkMode == "linked_file" || a.linkMode == "linked_url")
        return fail(err, "This is a linked file, which the Kindle doesn't have, so its text can't be read.", "linkMode " + a.linkMode),
               std::nullopt;
    DocType type;
    if (a.contentType == "application/pdf") type = DocType::Pdf;
    else if (a.contentType == "application/epub+zip") type = DocType::Epub;
    else
        return fail(err, "Reading the text is only for EPUB and PDF files.", "content type " + a.contentType), std::nullopt;
    std::error_code ec;
    if (a.state != AttachmentRecord::State::Local || a.localPath.empty() || !fs::is_regular_file(a.localPath, ec))
        return fail(err, "This file isn't on the Kindle - open it once to download it.", "no local file for " + a.key), std::nullopt;

    // Without a checksum there is nothing to tell a changed file by: no cache.
    const bool cacheable = safeName(a.key) && safeName(a.md5);
    const fs::path dir = fs::path(pluginsDir) / ".cache" / "fulltext";
    const fs::path entry = dir / (a.key + "-" + a.md5 + ".txt");
    if (cacheable) {
        if (auto hit = readAll(entry); hit && hit->size() <= kMaxFullTextBytes) {
            fs::last_write_time(entry, fs::file_time_type::clock::now(), ec);  // used now (least recently used go first)
            return hit;
        }
    }

    UserError e;
    auto engine = loader ? loader(type, env, &e) : loadEngine(type, env, &e);
    if (!engine) return fail(err, e.message.empty() ? "The reader for this file couldn't be started." : e.message, e.details), std::nullopt;
    if (!engine->openForText(a.localPath, &e)) {
        if (err) *err = e;
        return std::nullopt;
    }
    std::string text;
    if (!engine->fullText(text, &e)) {
        if (err) *err = e.message.empty() ? UserError{ErrorKind::Other, "This file's text can't be read.", "fullText failed"} : e;
        return std::nullopt;
    }
    engine.reset();
    if (text.size() > kMaxFullTextBytes)
        return fail(err, "This file's text is larger than 32 MB.", "full text over limit"), std::nullopt;

    if (cacheable) {
        fs::create_directories(dir, ec);
        const fs::path tmp = fs::path(entry).concat(".tmp");
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            ok = static_cast<bool>(out);
        }
        if (ok) fs::rename(tmp, entry, ec), ok = !ec;
        if (!ok) {
            fs::remove(tmp, ec);  // no cache then; the text itself is fine
        } else {
            const std::string prefix = a.key + "-", mine = entry.filename().string();
            fs::directory_iterator it(dir, ec);
            for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
                const std::string name = it->path().filename().string();
                std::error_code rc;
                if (name != mine && name.rfind(prefix, 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".txt")
                    fs::remove(it->path(), rc);
            }
            prune(dir, entry);
        }
    }
    return text;
}

}  // namespace

std::optional<std::string> extractFullText(const AttachmentRecord& a, const std::string& pluginsDir, const EngineEnv& env,
                                           UserError* err, const EngineLoader& loader) {
    try {
        return extract(a, pluginsDir, env, err, loader);
    } catch (const std::exception& e) {
        fail(err, "The app failed reading this file's text.", e.what());
    } catch (...) {
        fail(err, "The app failed reading this file's text.", "unknown exception");
    }
    return std::nullopt;
}

}  // namespace zk
