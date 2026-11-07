// zk-sync: the storage and sync layer without a UI, for real runs on the Mac and the Kindle.
//   zk-sync [--config DIR | --env FILE] [--db PATH] [--ca FILE] COMMAND
//   commands: pull | status | search <words> | collections | annotations <attachmentKey>
//             | fetch <attachmentKey> <dir> | highlights-offline on|off
//             | push | annotate-test <attachmentKey> [page] | annotate-test-edit <key>
//             | position-test <attachmentKey>
//             | annotate-test-cleanup
//   `annotations` downloads that book's annotations first, as opening a book in the app will.
//   `annotate-test` highlights the first text of a page (3 by default) of a fetched file, as the
//   reader would, tagged zk-test with a "[zk-test]" comment; `annotate-test-cleanup` deletes
//   those (and only those); `annotate-test-edit` makes one of them blue with an "edited" comment.
//   They change the local library; `push` sends the changes.
// Every request goes through the WriteGuard, which allows annotation writes and the reading positions
// of the library's own files, and nothing else.
// Credentials are never printed.

#include "docs/fonts.h"
#include "core/log.h"
#include "docs/engine_factory.h"
#include "net/http.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/config.h"
#include "sync/sync_engine.h"
#include "sync/webdav.h"
#include "sync/zotero_storage.h"
#include "zotero/api.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
const EngineEnv kEngineEnv{"/mnt/us/zotero/share/crengine", readingFontDirs(), "/mnt/us/zotero/cache/cre"};
#else
const char* kDefaultConfigDir = "local";
const char* kDefaultDb = "local/zk.sqlite";
const char* kDefaultCa = "";
const char* kDroppedConfig = "";
const EngineEnv kEngineEnv{"third_party/crengine/cr3gui/data", readingFontDirs(), "local/cache/cre"};
#endif

constexpr const char* kTestTag = "zk-test";
constexpr const char* kTestComment = "[zk-test] made by zk-sync annotate-test";

int usage() {
    std::fprintf(stderr,
                 "usage: zk-sync [--config DIR | --env FILE] [--db PATH] [--ca FILE] COMMAND\n"
                 "  pull | status | search <words> | collections | annotations <attachmentKey>\n"
                 "  | fetch <attachmentKey> <dir> | highlights-offline on|off\n"
                 "  | push | annotate-test <attachmentKey> [page] | annotate-test-edit <key>\n"
                 "  | annotate-test-cleanup\n");
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
    WriteGuard http(
        curl, [&](const std::string& key) { return lib.annotation(key).has_value(); },
        [&](const std::string& key) { return lib.attachment(key).has_value(); });  // reading positions
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
            auto files = makeFileStore(http, *cfg);  // WebDAV, or Zotero storage without one
            const auto t0 = std::chrono::steady_clock::now();
            UserError err;
            if (!files->download(*a, args[2], &err)) {
                printError(err);
                return 1;
            }
            a->localPath = args[2] + "/" + a->key + "/" + localFileName(*a);
            a->state = AttachmentRecord::State::Local;
            lib.upsertAttachment(*a);
            std::printf("fetched %s (md5 ok) in %.1f s\n", a->localPath.c_str(),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        } else if (cmd == "push") {
            SyncEngine engine(api, lib);
            const int pending = engine.pendingChanges();
            SyncReport rep = engine.push();
            std::printf("push: %d change(s) pending, %d sent; conflicts %d; library version %lld\n", pending,
                        rep.annotations, rep.conflicts, static_cast<long long>(rep.toVersion));
            if (rep.error) {
                printError(*rep.error);
                return 1;
            }
        } else if (cmd == "position-test" && args.size() == 2) {
            // Zotero's reading position for a fetched file -> its page here -> one page on -> sent back.
            auto a = lib.attachment(args[1]);
            if (!a || a->state != AttachmentRecord::State::Local || a->localPath.empty()) {
                std::printf("error: %s is not downloaded (run fetch first).\n", args[1].c_str());
                return 1;
            }
            UserError err;
            auto engine = loadEngine(a->contentType == "application/pdf" ? DocType::Pdf : DocType::Epub, kEngineEnv, &err);
            if (!engine || !engine->open(a->localPath, &err)) {
                printError(err);
                return 1;
            }
            engine->setLayout(Layout{});
            auto rp = lib.readingPosition(a->key);
            const std::string remote = rp ? rp->remote : "";
            const int page = remote.empty() ? 0 : engine->pageOfZoteroPosition(remote);
            std::printf("Zotero's position: %s -> page %d of %d\n", remote.empty() ? "(none)" : remote.c_str(), page + 1,
                        engine->pageCount());
            if (page < 0) return 1;
            const std::string next = engine->zoteroPosition(page + 1);
            std::printf("one page on: %s (back to page %d)\n", next.c_str(), engine->pageOfZoteroPosition(next) + 1);
            lib.setLocalPosition(a->key, next);
            SyncEngine sync(api, lib);
            SyncReport rep = sync.push();
            if (rep.error) {
                printError(*rep.error);
                return 1;
            }
            std::printf("sent; Zotero's setting is now version %lld\n",
                        static_cast<long long>(lib.readingPosition(a->key)->remoteVersion));
        } else if (cmd == "annotate-test" && (args.size() == 2 || args.size() == 3)) {
            auto a = lib.attachment(args[1]);
            if (!a || a->state != AttachmentRecord::State::Local || a->localPath.empty()) {
                std::printf("error: %s is not downloaded (run fetch first).\n", args[1].c_str());
                return 1;
            }
            const bool pdf = a->contentType == "application/pdf";
            UserError err;
            auto engine = loadEngine(pdf ? DocType::Pdf : DocType::Epub, kEngineEnv, &err);
            if (!engine || !engine->open(a->localPath, &err)) {
                printError(err);
                return 1;
            }
            const int page = (args.size() == 3 ? std::atoi(args[2].c_str()) : 3) - 1;
            std::optional<Selection> sel;
            // The first line of text from the top, wherever the page's margins are.
            auto good = [&] { return sel && !sel->text.empty() && sel->roundTrips; };
            for (int y = 120; y < 1400 && !good(); y += 20)
                for (int x : {200, 350, 500}) {
                    sel = engine->select(page, {x, y}, {x + 500, y});
                    if (good()) break;
                }
            if (!sel || sel->text.empty() || !sel->roundTrips) {
                std::printf("error: No selectable text on page %d.\n", page + 1);
                return 1;
            }
            AnnotationRecord n;
            n.parentKey = a->key;
            n.type = "highlight";
            n.text = sel->text;
            n.comment = kTestComment;
            n.color = "#ffd400";
            n.tags = {kTestTag};
            n.sortIndex = sel->sortIndex;
            n.position = sel->position;
            n.pageLabel = sel->pageLabel;
            const std::string key = lib.createAnnotation(n);
            std::printf("created %s on page %d: \"%s\"\n  position %s\n  sortIndex %s (run push to send it)\n",
                        key.c_str(), page + 1, sel->text.c_str(), sel->position.c_str(), sel->sortIndex.c_str());
        } else if (cmd == "annotate-test-edit" && args.size() == 2) {
            auto a = lib.annotation(args[1]);
            const bool ours = a && std::find(a->tags.begin(), a->tags.end(), kTestTag) != a->tags.end() &&
                              a->comment.rfind("[zk-test]", 0) == 0;
            if (!ours) {
                std::printf("error: %s is not one of this tool's zk-test annotations.\n", args[1].c_str());
                return 1;
            }
            a->color = "#2ea8e5";
            a->comment = "[zk-test] edited on the Kindle side";
            lib.editAnnotation(*a);
            std::printf("edited %s: blue, comment \"%s\" (run push to send it)\n", a->key.c_str(), a->comment.c_str());
        } else if (cmd == "select-bench" && (args.size() == 2 || args.size() == 3)) {
            // How long a selection takes on this device, as a reader drag asks for it.
            auto a = lib.attachment(args[1]);
            if (!a || a->state != AttachmentRecord::State::Local) {
                std::printf("error: %s is not downloaded.\n", args[1].c_str());
                return 1;
            }
            UserError err;
            auto engine = loadEngine(a->contentType == "application/pdf" ? DocType::Pdf : DocType::Epub, kEngineEnv, &err);
            if (!engine || !engine->open(a->localPath, &err)) {
                printError(err);
                return 1;
            }
            const int page = args.size() == 3 ? std::atoi(args[2].c_str()) : 20;
            auto ms = [](auto t0) {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            };
            Point from{0, 0};
            const auto first = std::chrono::steady_clock::now();
            for (int y = 150; y < 1300 && !from.x; y += 20)
                for (int x : {200, 350, 500})
                    if (auto s = engine->select(page, {x, y}, {x, y}); s && !s->text.empty()) {
                        from = {x, y};
                        break;
                    }
            if (!from.x) {
                std::printf("error: no text on page %d\n", page + 1);
                return 1;
            }
            std::printf("first selection on the page (chapter not yet parsed): %.1f ms\n", ms(first));
            double total = 0, worst = 0;
            const int steps = 20;
            for (int i = 1; i <= steps; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                auto s = engine->select(page, from, {from.x + 30 * i, from.y + 25 * i});
                const double t = ms(t0);
                total += t;
                worst = std::max(worst, t);
                if (i == steps && s) std::printf("last selection: %zu chars, %zu rects\n", s->text.size(), s->rects.size());
            }
            std::printf("select on page %d: %d drag steps, mean %.1f ms, worst %.1f ms\n", page + 1, steps, total / steps,
                        worst);
        } else if (cmd == "annotate-test-cleanup") {
            int n = 0;
            for (const auto& row : lib.recentAnnotations(0, 1'000'000)) {
                const AnnotationRecord& a = row.annotation;
                const bool tagged = std::find(a.tags.begin(), a.tags.end(), kTestTag) != a.tags.end();
                if (!tagged || a.comment.rfind("[zk-test]", 0) != 0) continue;  // only this tool's own
                lib.deleteAnnotation(a.key);
                std::printf("deleting %s \"%s\"\n", a.key.c_str(), a.text.substr(0, 60).c_str());
                ++n;
            }
            std::printf("%d test annotation(s) marked for deletion (run push to send)\n", n);
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
