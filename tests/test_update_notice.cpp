#include <doctest/doctest.h>

#include "fake_http.h"
#include "fn_http.h"
#include "store/db.h"
#include "store/library.h"
#include "update/update_notice.h"
#include "update/updater.h"

#include <atomic>
#include <memory>
#include <string>

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kHour = 3600 * 1000LL;

// GitHub's latest release, as an Updater on 0.2.0 sees it; every request counted.
struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    std::shared_ptr<std::string> tag = std::make_shared<std::string>("v0.3.0");
    std::shared_ptr<bool> down = std::make_shared<bool>(false);
    std::shared_ptr<std::atomic<int>> requests = std::make_shared<std::atomic<int>>(0);
    int64_t now = 1'762'500'000'000;
    std::unique_ptr<Updater> updater;
    std::unique_ptr<UpdateNotice> notice;
    explicit Rig(Version current = {0, 2, 0}) {
        UpdaterConfig c;
        auto tag_ = tag;
        auto down_ = down;
        auto requests_ = requests;
        c.makeHttp = [tag_, down_, requests_] {
            return std::make_unique<FnHttp>([tag_, down_, requests_](const HttpRequest&) {
                ++*requests_;
                if (*down_) return FakeHttp::status(503);
                return FakeHttp::ok(R"({"tag_name":")" + *tag_ + R"(","body":"Notes","assets":[]})");
            });
        };
        c.current = current;
        c.probe = [] { return NetState{}; };
        updater = std::make_unique<Updater>(c);
        notice = std::make_unique<UpdateNotice>(*updater, [this] { return now; });
    }
    // A sync finished; the check (if one started) answered and was noted.
    void synced(bool ok = true) {
        notice->onSyncFinished(lib, ok);
        updater->wait();
        notice->poll(lib);
    }
};

}  // namespace

TEST_CASE("update notice: checks after a successful sync, then not for an hour") {
    Rig rig;
    rig.synced();
    CHECK(*rig.requests == 1);
    rig.synced();
    rig.now += 50 * 60 * 1000LL;
    rig.synced();
    CHECK(*rig.requests == 1);
    rig.now += 10 * 60 * 1000LL + 1;
    rig.synced();
    CHECK(*rig.requests == 2);
}

TEST_CASE("update notice: no check after a failed sync") {
    Rig rig;
    rig.synced(false);
    CHECK(*rig.requests == 0);
}

TEST_CASE("update notice: a failed check tries again an hour later, not before") {
    Rig rig;
    *rig.down = true;
    rig.synced();
    CHECK(*rig.requests == 1);
    CHECK_FALSE(rig.notice->available(rig.lib));
    rig.now += 30 * 60 * 1000LL;
    rig.synced();
    CHECK(*rig.requests == 1);
    rig.now += kHour;
    rig.synced();
    CHECK(*rig.requests == 2);
}

TEST_CASE("update notice: names the newer version; dismissed, it waits for a newer one") {
    Rig rig;
    rig.synced();
    REQUIRE(rig.notice->available(rig.lib));
    CHECK(rig.notice->available(rig.lib)->str() == "0.3.0");
    rig.notice->dismiss(rig.lib);
    CHECK_FALSE(rig.notice->available(rig.lib));
    CHECK(rig.notice->pending(rig.lib)->str() == "0.3.0");  // Settings still says so
    *rig.tag = "v0.4.0";
    rig.now += 2 * kHour;
    rig.synced();
    REQUIRE(rig.notice->available(rig.lib));
    CHECK(rig.notice->available(rig.lib)->str() == "0.4.0");
}

TEST_CASE("update notice: remembered across restarts, gone once installed") {
    Rig rig;
    rig.synced();
    UpdateNotice again(*rig.updater, [&] { return rig.now; });
    CHECK(again.available(rig.lib)->str() == "0.3.0");
    Rig updated({0, 3, 0});
    updated.lib.setMetaValue("update:available", "0.3.0");
    CHECK_FALSE(updated.notice->available(updated.lib));
    CHECK_FALSE(updated.notice->pending(updated.lib));
}

TEST_CASE("update notice: up to date clears an old notice") {
    Rig rig;
    rig.synced();
    REQUIRE(rig.notice->available(rig.lib));
    *rig.tag = "v0.2.0";  // the release was pulled
    rig.now += 2 * kHour;
    rig.synced();
    CHECK_FALSE(rig.notice->available(rig.lib));
}

TEST_CASE("update notice: a check from Settings counts as the week's check") {
    Rig rig;
    rig.updater->check();
    rig.updater->wait();
    rig.notice->poll(rig.lib);
    CHECK(rig.notice->available(rig.lib)->str() == "0.3.0");
    rig.synced();
    CHECK(*rig.requests == 1);
}

TEST_CASE("update notice: a clock set back doesn't stop checks forever") {
    Rig rig;
    rig.synced();
    rig.now -= 400 * 24 * kHour;  // the Kindle's clock was wrong, then fixed backwards
    rig.synced();
    rig.now += kHour + 1;
    rig.synced();
    CHECK(*rig.requests == 2);
}
