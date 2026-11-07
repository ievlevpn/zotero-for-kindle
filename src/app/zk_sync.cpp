// zk-sync: the storage and sync layer without a UI, for real runs on the Mac and the Kindle.
//   zk-sync [--config DIR | --env FILE] [--db PATH] [--ca FILE] COMMAND
//   commands: pull | status | search <words> | collections | annotations <attachmentKey>
//             | fetch <attachmentKey> <dir> | highlights-offline on|off
//   `annotations` downloads that book's annotations first, as opening a book in the app will.
// Reads only: there is deliberately no push command yet. Every request goes through the
// WriteGuard, which allows annotation writes and nothing else. Credentials are never printed.

#include "core/log.h"
#include "net/http.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/config.h"
#include "sync/sync_engine.h"
#include "sync/webdav.h"
#include "zotero/api.h"

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace zk;

namespace {

#ifdef ZK_KINDLE
const char* kDefaultConfigDir = "/var/local/zotero";
const char* kDefaultDb = "/mnt/us/zotero/zotero.db";  // see research/spikes/s6-sqlite-fuse.md
const char* kDefaultCa = "/mnt/us/zotero/share/cacert.pem";
const char* kDroppedConfig = "/mnt/us/zotero/config.json";
#else
const char* kDefaultConfigDir = "local";
const char* kDefaultDb = "local/zk.sqlite";
const char* kDefaultCa = "";
const char* kDroppedConfig = "";
#endif

int usage() {
    std::fprintf(stderr,
                 "usage: zk-sync [--config DIR | --env FILE] [--db PATH] [--ca FILE] COMMAND\n"
                 "  pull | status | search <words> | collections | annotations <attachmentKey>\n"
                 "  | fetch <attachmentKey> <dir> | highlights-offline on|off\n");
    return 2;
}

void printError(const UserError& e) {
    std::printf("error: %s\n  details: %s\n", e.message.c_str(), e.details.c_str());
}

long peakRssKb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
}

std::string creatorLine(const ItemRecord& r) {
    std::string s;
    for (const auto& c : r.creators) {
        if (!s.empty()) {
            s += " et al.";
            break;
        }
        s = !c.last.empty() ? c.last : c.name;
    }
    return s;
}

void printItems(const std::vector<ItemRecord>& items) {
    for (const auto& r : items)
        std::printf("%s  %-14s %s — %s %s\n", r.key.c_str(), r.itemType.c_str(), r.title.empty() ? "(untitled)" : r.title.c_str(),
                    creatorLine(r).c_str(), r.date.c_str());
}

void printCollections(const std::vector<CollectionRecord>& all, const std::string& parent, int depth) {
    for (const auto& c : all)
        if (c.parentKey == parent) {
            std::printf("%*s%s  %s\n", depth * 2, "", c.key.c_str(), c.name.c_str());
            printCollections(all, c.key, depth + 1);
        }
}

}  // namespace

int main(int argc, char** argv) {
    std::string configDir = kDefaultConfigDir, envFile, dbPath = kDefaultDb, ca = kDefaultCa;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (a == "--config") configDir = next();
        else if (a == "--env") envFile = next();
        else if (a == "--db") dbPath = next();
        else if (a == "--ca") ca = next();
        else args.push_back(a);
    }
    if (args.empty()) return usage();
    const std::string cmd = args[0];

    std::optional<Config> cfg;
    if (!envFile.empty()) {
        cfg = configFromEnvFile(envFile);
    } else {
        if (*kDroppedConfig) importDroppedConfig(kDroppedConfig, configDir);
        cfg = loadConfig(configDir);
    }
    if (!cfg || cfg->userId <= 0 || cfg->apiKey.empty()) {
        std::printf("error: No Zotero account configured.\n  details: put config.json in %s or pass --env FILE\n",
                    envFile.empty() ? configDir.c_str() : envFile.c_str());
        return 1;
    }

    std::optional<Db> db;
    try {
        db.emplace(Db::open(dbPath));
    } catch (const DbError& e) {
        printError({ErrorKind::Other, "Can't open the library database.", e.what()});
        return 1;
    }
    Library lib(*db);
    CurlHttp curl(ca);
    WriteGuard http(curl, [&](const std::string& key) {
        return lib.annotation(key).has_value();
    });
    ZoteroApi api(http, cfg->userId, cfg->apiKey);
    api.setOnWait([](const UserError& e) { std::printf("%s\n", e.message.c_str()); });

    try {
        if (cmd == "pull") {
            const auto t0 = std::chrono::steady_clock::now();
            SyncEngine engine(api, lib);
            std::string lastPhase;
            SyncReport rep = engine.pull([&](const SyncProgress& p) {
                if (p.phase != lastPhase || p.done == p.total || p.done % 1000 < 50) {
                    std::printf("  %-12s %6d / %d\n", p.phase.c_str(), p.done, p.total);
                    std::fflush(stdout);
                }
                lastPhase = p.phase;
            });
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("pull: version %lld -> %lld; items +%d ~%d -%d; annotations %d; conflicts %d; %.1f s; peak RSS %ld KB\n",
                        static_cast<long long>(rep.fromVersion), static_cast<long long>(rep.toVersion), rep.itemsAdded,
                        rep.itemsUpdated, rep.itemsRemoved, rep.annotations, rep.conflicts, secs, peakRssKb());
            if (rep.error) {
                printError(*rep.error);
                return 1;
            }
        } else if (cmd == "status") {
            LibraryStats s = lib.stats();
            std::printf("highlights: %s (%zu books opened)\n",
                        lib.allAnnotationsOffline() ? "all offline" : "on demand", lib.trackedAttachments().size());
            std::printf("library version %lld\nitems %lld (incl. child items)\nattachments %lld\nannotations %lld\n"
                        "collections %lld\nconflicts %lld\nunsent %lld\n",
                        static_cast<long long>(lib.libraryVersion()), static_cast<long long>(s.items),
                        static_cast<long long>(s.attachments), static_cast<long long>(s.annotations),
                        static_cast<long long>(s.collections), static_cast<long long>(s.conflicts),
                        static_cast<long long>(s.unsent));
        } else if (cmd == "search" && args.size() >= 2) {
            std::string q;
            for (size_t i = 1; i < args.size(); ++i) q += (i > 1 ? " " : "") + args[i];
            printItems(lib.search(q, 25));
        } else if (cmd == "collections") {
            printCollections(lib.collections(), "", 0);
        } else if (cmd == "highlights-offline" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
            SyncEngine(api, lib).setAllHighlightsOffline(args[1] == "on");
            std::printf("highlights: %s%s\n", args[1] == "on" ? "all offline" : "on demand",
                        args[1] == "on" ? " (downloaded at the next pull)" : "");
        } else if (cmd == "annotations" && args.size() == 2) {
            const auto t0 = std::chrono::steady_clock::now();
            SyncReport rep = SyncEngine(api, lib).pullAnnotationsFor(args[1], nullptr);
            std::printf("synced %d annotation(s) in %.1f s\n", rep.annotations,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            if (rep.error) printError(*rep.error);
            for (const auto& a : lib.annotationsFor(args[1]))
                std::printf("%s  %-9s %s  %s%s%s\n", a.key.c_str(), a.type.c_str(), a.sortIndex.c_str(),
                            a.text.substr(0, 80).c_str(), a.comment.empty() ? "" : "  // ", a.comment.substr(0, 60).c_str());
        } else if (cmd == "fetch" && args.size() == 3) {
            auto a = lib.attachment(args[1]);
            if (!a) {
                std::printf("error: No attachment %s in the local library (run pull first).\n", args[1].c_str());
                return 1;
            }
            if (cfg->webdavUrl.empty()) {
                std::printf("error: No WebDAV server configured.\n");
                return 1;
            }
            WebDav dav(http, cfg->webdavUrl, cfg->webdavUser, cfg->webdavPassword);
            const auto t0 = std::chrono::steady_clock::now();
            UserError err;
            if (!dav.download(*a, args[2], &err)) {
                printError(err);
                return 1;
            }
            a->localPath = args[2] + "/" + a->key + "/" + localFileName(*a);
            a->state = AttachmentRecord::State::Local;
            lib.upsertAttachment(*a);
            std::printf("fetched %s (md5 ok) in %.1f s\n", a->localPath.c_str(),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        } else {
            return usage();
        }
    } catch (const DbError& e) {
        printError(lib.lastError().kind == ErrorKind::None ? UserError{ErrorKind::Other, "Database error.", e.what()}
                                                           : lib.lastError());
        return 1;
    }
    return 0;
}
