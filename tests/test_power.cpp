#include <doctest/doctest.h>

#include "app/paths.h"
#include "fake_lipc.h"
#include "platform/power.h"

#include <filesystem>
#include <stdexcept>
#include <unistd.h>

using namespace zk;
using zk::test::FakeLipc;

TEST_CASE("keep-awake: nested acquire sets the property once and releases on the last release") {
    FakeLipc lipc;
    KeepAwake awake(lipc);
    awake.acquire();
    awake.acquire();
    awake.release();
    CHECK(lipc.sets == std::vector<std::string>{"com.lab126.powerd preventScreenSaver 1"});
    awake.release();
    CHECK(lipc.sets == std::vector<std::string>{"com.lab126.powerd preventScreenSaver 1",
                                                "com.lab126.powerd preventScreenSaver 0"});
    awake.release();  // unbalanced release is ignored
    CHECK(lipc.sets.size() == 2);
}

TEST_CASE("keep-awake: the scope releases on throw") {
    FakeLipc lipc;
    KeepAwake awake(lipc);
    try {
        KeepAwakeScope scope(awake);
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
    }
    CHECK(lipc.sets.back() == "com.lab126.powerd preventScreenSaver 0");
}

TEST_CASE("storageAvailable: a writable directory yes, a removed one no") {
    char tmpl[] = "/tmp/zk-store-XXXXXX";
    std::filesystem::path dir = mkdtemp(tmpl);
    CHECK(storageAvailable(dir.string()));
    std::filesystem::remove_all(dir);
    CHECK_FALSE(storageAvailable(dir.string()));
}

TEST_CASE("paths: desktop defaults live under local/") {
    Paths p = defaultPaths();
#ifdef ZK_KINDLE
    CHECK(p.dbPath == "/mnt/us/zotero/zotero.db");
#else
    CHECK(p.configDir == "local");
    CHECK(p.dbPath == "local/zk.sqlite");
    CHECK(p.filesDir == "local/files");
    CHECK(p.droppedConfig.empty());
#endif
}
