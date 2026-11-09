#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "fake_http.h"
#include "fake_plugins.h"
#include "fn_http.h"
#include "plugins/host.h"
#include "plugins/plugin_http.h"

using namespace zk;
using namespace zk::test;
using zk::plugins::FetchError;
using zk::plugins::PluginHttp;

namespace {

const PluginEvent kRun{"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

PluginManifest manifest(std::set<Permission> perms, std::vector<std::string> hosts = {}) {
    PluginManifest m;
    m.id = "p";
    m.name = "P";
    m.permissions = std::move(perms);
    m.hosts = std::move(hosts);
    if (!m.hosts.empty()) m.permissions.insert(Permission::Net);
    return m;
}

HttpRequest get(std::string url) {
    HttpRequest r;
    r.url = std::move(url);
    return r;
}

// What PluginHttp::send threw (kind None if it didn't).
struct Outcome {
    bool threw = false;
    FetchError::Kind kind{};
    std::string message, detail;
    HttpResponse resp;
};

Outcome send(PluginHttp& ph, const PluginManifest& m, HttpRequest r, int timeoutMs = 30000) {
    Outcome o;
    try {
        o.resp = ph.send(m, std::move(r), timeoutMs);
    } catch (const FetchError& e) {
        o.threw = true;
        o.kind = e.kind;
        o.message = e.what();
        o.detail = e.detail;
    }
    return o;
}

HttpResponse redirect(long code, std::string location) { return FakeHttp::status(code, {{"location", std::move(location)}}); }

std::string pluginWith(const std::string& body) {
    return "const out = (x) => console.log('OUT ' + (typeof x === 'string' ? x : x === undefined ? 'undefined' : JSON.stringify(x)));\n"
           "zk.on('syncFinished', async ({ initial }) => { try {\n" + body +
           "\n} catch (e) { console.log('ERR ' + e.name + ' ' + e.message + (e.permission ? ' perm=' + e.permission : '') + "
           "(e.limit ? ' limit=' + e.limit : '')); } });\n";
}

// A FakeHttp shared by every network worker (behind a lock).
struct SharedFake {
    std::mutex mu;
    FakeHttp fake;
    std::function<std::unique_ptr<Http>()> maker() {
        return [this] {
            return std::make_unique<FnHttp>([this](const HttpRequest& r) {
                std::lock_guard<std::mutex> lock(mu);
                return fake.send(r);
            });
        };
    }
};

std::string runOnce(FakePlugins& fp, PluginHostDeps deps, const std::string& id) {
    PluginHost host(std::move(deps));
    host.setEnabled(id, true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    return fp.log(id);
}

}  // namespace

// ---- The guard ------------------------------------------------------------------------------

TEST_CASE("fetch: a declared host succeeds; an undeclared one is a PermissionError naming net:<host>") {
    FakeHttp fake;
    fake.expect("GET", "https://export.arxiv.org/rss", FakeHttp::ok("<rss/>"));
    fake.expect("GET", "https://a.example.org/", FakeHttp::ok("sub"));
    PluginHttp ph(fake, "dav.example.net");
    const auto m = manifest({}, {"export.arxiv.org", "*.example.org"});

    Outcome ok = send(ph, m, get("https://export.arxiv.org/rss/math.PR"));
    REQUIRE_FALSE(ok.threw);
    CHECK(ok.resp.status == 200);
    CHECK(ok.resp.body == "<rss/>");
    CHECK(ok.resp.url == "https://export.arxiv.org/rss/math.PR");
    CHECK_FALSE(send(ph, m, get("https://a.example.org/x")).threw);
    Outcome upper = send(ph, m, get("https://EXPORT.arxiv.org./rss2"));  // case and a final dot don't matter
    CHECK_FALSE(upper.threw);
    CHECK(fake.sent.back().url == "https://export.arxiv.org/rss2");

    Outcome no = send(ph, m, get("https://api.deepl.com/v2/translate"));
    REQUIRE(no.threw);
    CHECK(no.kind == FetchError::Kind::Permission);
    CHECK(no.detail == "net:api.deepl.com");
    CHECK(no.message == "zk.fetch(\"https://api.deepl.com/…\")");
    Outcome bare = send(ph, m, get("https://example.org/"));  // *.example.org doesn't cover example.org
    CHECK(bare.kind == FetchError::Kind::Permission);
    CHECK(bare.detail == "net:example.org");
    CHECK(fake.sent.size() == 3);  // nothing sent for the refused ones
}

TEST_CASE("fetch: net:* reaches any https host") {
    FakeHttp fake;
    fake.expect("GET", "https://anything.test:8443/a?b=1", FakeHttp::ok("x"));
    PluginHttp ph(fake, "");
    Outcome o = send(ph, manifest({Permission::NetAny}), get("https://anything.test:8443/a?b=1"));
    REQUIRE_FALSE(o.threw);
    CHECK(o.resp.body == "x");
    REQUIRE(fake.sent.size() == 1);
    CHECK(fake.sent[0].url == "https://anything.test:8443/a?b=1");
}

TEST_CASE("fetch: http:, api.zotero.org, zotero.org and the WebDAV host are refused even with net:*") {
    FakeHttp fake;
    PluginHttp ph(fake, "dav.example.net");
    const auto any = manifest({Permission::NetAny});
    for (const char* url : {"http://example.org/", "https://api.zotero.org/users/1/items", "https://zotero.org/",
                            "https://ZOTERO.org./", "https://www.zotero.org/x", "https://api.zotero.org:443/x",
                            "https://dav.example.net/zotero/", "https://DAV.example.net.:8443/", "ftp://example.org/",
                            "https://user:pw@example.org/", "https://ex%61mple.org/", "not a url"}) {
        CAPTURE(url);
        Outcome o = send(ph, any, get(url));
        REQUIRE(o.threw);
        CHECK(o.kind == FetchError::Kind::Refused);
    }
    // Declaring them doesn't help either.
    Outcome o = send(ph, manifest({}, {"api.zotero.org", "*.zotero.org"}), get("https://api.zotero.org/"));
    CHECK(o.kind == FetchError::Kind::Refused);
    CHECK(contains(o.message, "api.zotero.org"));
    CHECK(fake.sent.empty());

    // The WebDAV host may be configured as a URL.
    PluginHttp ph2(fake, "https://Dav.Other.net:8080/remote.php/dav");
    CHECK(send(ph2, any, get("https://dav.other.net/")).kind == FetchError::Kind::Refused);
    CHECK(fake.sent.empty());
}

TEST_CASE("fetch: a redirect to api.zotero.org or to http: is refused at the redirect") {
    const auto m = manifest({}, {"a.example.org", "b.example.org"});
    for (const char* to : {"https://api.zotero.org/keys/current", "http://b.example.org/x", "//zotero.org/x",
                           "https://dav.example.net/"}) {
        CAPTURE(to);
        FakeHttp fake;
        fake.expect("GET", "https://a.example.org/", redirect(302, to));
        fake.expect("GET", "", FakeHttp::ok("must not be reached"));
        PluginHttp ph(fake, "dav.example.net");
        Outcome o = send(ph, manifest({Permission::NetAny}), get("https://a.example.org/start"));
        REQUIRE(o.threw);
        CHECK(o.kind == FetchError::Kind::Refused);
        CHECK(contains(o.message, "redirect"));
        CHECK(fake.sent.size() == 1);
        for (const auto& r : fake.sent) CHECK_FALSE(r.followRedirects);  // curl never follows on its own
    }
    // An undeclared host at a redirect: PermissionError naming it.
    FakeHttp fake;
    fake.expect("GET", "https://a.example.org/", redirect(301, "https://c.example.org/y"));
    PluginHttp ph(fake, "");
    Outcome o = send(ph, m, get("https://a.example.org/start"));
    CHECK(o.kind == FetchError::Kind::Permission);
    CHECK(o.detail == "net:c.example.org");
    CHECK(contains(o.message, "redirect"));
    CHECK(fake.sent.size() == 1);

    // Allowed hops are followed (relative too), up to 5.
    FakeHttp f2;
    f2.expect("GET", "https://a.example.org/1", redirect(302, "/2"));
    f2.expect("GET", "https://a.example.org/2", redirect(307, "https://b.example.org/3"));
    f2.expect("GET", "https://b.example.org/3", FakeHttp::ok("end"));
    PluginHttp ph2(f2, "");
    Outcome ok = send(ph2, m, get("https://a.example.org/1"));
    REQUIRE_FALSE(ok.threw);
    CHECK(ok.resp.body == "end");
    CHECK(ok.resp.url == "https://b.example.org/3");

    FakeHttp f3;
    for (int i = 0; i < 7; ++i) f3.expect("GET", "https://a.example.org/", redirect(302, "/loop" + std::to_string(i)));
    PluginHttp ph3(f3, "");
    Outcome loop = send(ph3, m, get("https://a.example.org/"));
    REQUIRE(loop.threw);
    CHECK(contains(loop.message, "redirect"));
    CHECK(f3.sent.size() == 6);  // the first request and 5 redirects
}

TEST_CASE("fetch: Cookie and Authorization from the app are never added") {
    FakeHttp fake;
    fake.expect("GET", "https://a.example.org/", FakeHttp::ok("1"));
    fake.expect("POST", "https://a.example.org/", redirect(303, "/seen"));
    fake.expect("GET", "https://a.example.org/seen", redirect(302, "https://b.example.org/next"));
    fake.expect("GET", "https://b.example.org/next", FakeHttp::ok("2"));
    PluginHttp ph(fake, "");
    const auto m = manifest({}, {"a.example.org", "b.example.org"});

    HttpRequest plain = get("https://a.example.org/");
    plain.user = "app-user";  // whatever a caller leaves in the request, the guard sends none of it
    plain.password = "secret";
    plain.outFile = "/tmp/never";
    REQUIRE_FALSE(send(ph, m, plain).threw);
    REQUIRE(fake.sent.size() == 1);
    const HttpRequest& s = fake.sent[0];
    CHECK(s.user.empty());
    CHECK(s.password.empty());
    CHECK(s.outFile.empty());
    CHECK_FALSE(s.followRedirects);
    CHECK(s.maxBodyBytes == plugins::kFetchMaxBytes);
    CHECK(s.headers.empty());

    // The plugin's own headers go as set (but never Cookie or hop-by-hop ones); its Authorization is
    // dropped on the way to another host, and a 303 turns the POST into a GET without a body.
    HttpRequest own;
    own.method = "POST";
    own.url = "https://a.example.org/post";
    own.body = "{}";
    own.headers = {{"Authorization", "Bearer mine"}, {"Content-Type", "application/json"}, {"X-Two", "a"},
                   {"X-Two", "b"}, {"Cookie", "c=1"}, {"Host", "api.zotero.org"}, {"Transfer-Encoding", "chunked"}};
    Outcome o = send(ph, m, own);
    REQUIRE_FALSE(o.threw);
    CHECK(o.resp.body == "2");
    REQUIRE(fake.sent.size() == 4);
    auto has = [](const HttpRequest& r, const std::string& name) {
        for (const auto& [k, v] : r.headers) {
            std::string l = k;
            for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (l == name) return true;
        }
        return false;
    };
    const HttpRequest& post = fake.sent[1];
    CHECK(post.method == "POST");
    CHECK(post.body == "{}");
    CHECK(has(post, "authorization"));
    CHECK(has(post, "content-type"));
    CHECK(std::count_if(post.headers.begin(), post.headers.end(), [](auto& h) { return h.first == "X-Two"; }) == 2);
    CHECK_FALSE(has(post, "cookie"));
    CHECK_FALSE(has(post, "host"));
    CHECK_FALSE(has(post, "transfer-encoding"));
    const HttpRequest& seen = fake.sent[2];
    CHECK(seen.method == "GET");
    CHECK(seen.body.empty());
    CHECK_FALSE(has(seen, "content-type"));
    CHECK(has(seen, "authorization"));  // same host
    const HttpRequest& next = fake.sent[3];
    CHECK_FALSE(has(next, "authorization"));  // another host
    for (const auto& r : fake.sent) {
        CHECK(r.user.empty());
        CHECK_FALSE(has(r, "cookie"));
        CHECK_FALSE(has(r, "zotero-api-key"));
    }
}

TEST_CASE("fetch: a response over 10 MB fails with LimitError") {
    FakeHttp fake;
    fake.expect("GET", "https://a.example.org/big", FakeHttp::ok(std::string(plugins::kFetchMaxBytes + 1, 'x')));
    fake.expect("GET", "https://a.example.org/edge", FakeHttp::ok(std::string(plugins::kFetchMaxBytes, 'x')));
    HttpResponse cut;  // what CurlHttp returns when it stops reading at the cap
    cut.status = 200;
    cut.curlCode = 23;
    cut.tooLarge = true;
    fake.expect("GET", "https://a.example.org/cut", cut);
    PluginHttp ph(fake, "");
    const auto m = manifest({}, {"a.example.org"});
    Outcome big = send(ph, m, get("https://a.example.org/big"));
    REQUIRE(big.threw);
    CHECK(big.kind == FetchError::Kind::Limit);
    CHECK(big.detail == "responseSize");
    CHECK(contains(big.message, "10 MB"));
    CHECK_FALSE(send(ph, m, get("https://a.example.org/edge")).threw);
    Outcome c = send(ph, m, get("https://a.example.org/cut"));
    CHECK(c.kind == FetchError::Kind::Limit);
    CHECK(c.detail == "responseSize");
}

TEST_CASE("fetch: a request over its time fails with LimitError responseTime; the time reaches the client") {
    std::vector<HttpRequest> seen;
    FnHttp slow([&](const HttpRequest& r) {
        seen.push_back(r);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        HttpResponse resp;
        resp.curlCode = 28;  // CURLE_OPERATION_TIMEDOUT
        resp.curlError = "Operation timed out";
        return resp;
    });
    PluginHttp ph(slow, "");
    Outcome o = send(ph, manifest({}, {"a.example.org"}), get("https://a.example.org/"), 100);
    REQUIRE(o.threw);
    CHECK(o.kind == FetchError::Kind::Limit);
    CHECK(o.detail == "responseTime");
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].timeoutMs > 0);
    CHECK(seen[0].timeoutMs <= 100);
}

// ---- zk.fetch -------------------------------------------------------------------------------

TEST_CASE("fetch: zk.fetch resolves a Response (headers, text, json, bytes) and refuses what it must") {
    FakePlugins fp;
    SharedFake net;
    net.fake.expect("GET", "https://api.example.org/v1", FakeHttp::ok("{\"answer\":\"h\xC3\xA9\",\"bad\":\"\xFF\"}",
                                                                      {{"content-type", "application/json"},
                                                                       {"x-multi", "a, b"},
                                                                       {"etag", "\"1\""}}));
    net.fake.expect("POST", "https://api.example.org/v1/explain", FakeHttp::status(404));
    fp.add("a", pluginWith(R"(
        const res = await zk.fetch("https://api.example.org/v1");
        out([res.ok, res.status, res.url]);
        out([res.headers.get("Content-Type"), res.headers.get("X-MULTI"), res.headers.get("nope"), res.headers.has("ETag")]);
        out([...res.headers]);
        out([...res.headers.entries()].map(([n]) => n));
        out(await res.json());
        out((await res.text()).length);
        const b = await res.bytes();
        out(b instanceof Uint8Array && b.length);
        b[0] = 0;
        out((await res.bytes())[0]);            // a fresh copy each time
        const r2 = await zk.fetch("https://api.example.org/v1/explain", { method: "post", body: "{}",
          headers: [["content-type", "application/json"], ["x-k", "1"]], timeout: 99999 });
        out([r2.ok, r2.status]);
        try { await zk.fetch("https://other.example.com/x"); } catch (e) { out(e instanceof zk.PermissionError); throw e; }
    )"),
           {"net:api.example.org"});
    auto deps = fp.deps();
    deps.makeHttp = net.maker();
    std::string log = runOnce(fp, deps, "a");
    CHECK(contains(log, R"(OUT [true,200,"https://api.example.org/v1"])"));
    CHECK(contains(log, R"(OUT ["application/json","a, b",null,true])"));
    CHECK(contains(log, R"(OUT [["content-type","application/json"],["etag","\"1\""],["x-multi","a, b"]])"));
    CHECK(contains(log, R"(OUT ["content-type","etag","x-multi"])"));
    CHECK(contains(log, "OUT {\"answer\":\"h\xC3\xA9\",\"bad\":\"\xEF\xBF\xBD\"}"));
    CHECK(contains(log, "OUT 25"));  // characters
    CHECK(contains(log, "OUT 26"));  // bytes
    CHECK(contains(log, "OUT 123"));  // '{'
    CHECK(contains(log, "OUT [false,404]"));
    CHECK(contains(log, "OUT true"));
    CHECK(contains(log, "ERR PermissionError zk.fetch(\"https://other.example.com/…\") needs \"net:other.example.com\" in plugin.json "
                        "perm=net:other.example.com"));
    REQUIRE(net.fake.sent.size() == 2);
    const HttpRequest& post = net.fake.sent[1];
    CHECK(post.method == "POST");
    CHECK(post.body == "{}");
    CHECK(post.timeoutMs > 29000);
    CHECK(post.timeoutMs <= 30000);  // 99 999 counts as 30 000
    CHECK(post.headers.size() == 2);
}

TEST_CASE("fetch: zk.fetch's argument errors and the Cookie header reject with TypeError") {
    FakePlugins fp;
    SharedFake net;
    fp.add("a", pluginWith(R"(
        const tries = [
          () => zk.fetch("not a url"),
          () => zk.fetch("https://a.example.org/", { headers: { Cookie: "a=1" } }),
          () => zk.fetch("https://a.example.org/", { headers: [["x-a", "1\r\nHost: evil"]] }),
          () => zk.fetch("https://a.example.org/", { headers: [["bad name", "1"]] }),
          () => zk.fetch("https://a.example.org/", { method: "GET", body: "x" }),
          () => zk.fetch("https://a.example.org/", { method: "TRACE" }),
          () => zk.fetch("https://a.example.org/", { method: "POST", body: new Uint8Array(1) }),
          () => zk.fetch("https://a.example.org/", { timeout: -1 }),
        ];
        for (const t of tries) { try { await t(); out("no"); } catch (e) { out(e.name); } }
        try { await zk.fetch("http://a.example.org/"); } catch (e) { out(e.name + ": " + e.message); }
        try { await zk.fetch("https://a.example.org/", { method: "POST", body: "x".repeat(10 * 1024 * 1024 + 1) }); }
        catch (e) { out(e.name + ": " + e.message); }
    )"),
           {"net:a.example.org"});
    auto deps = fp.deps();
    deps.handlerBudgetMs = 3000;
    deps.makeHttp = net.maker();
    std::string log = runOnce(fp, deps, "a");
    CHECK_FALSE(contains(log, "OUT no"));
    size_t n = 0;
    for (size_t at = 0; (at = log.find("OUT TypeError", at)) != std::string::npos; ++at) ++n;
    CHECK(n == 8);
    CHECK(contains(log, "OUT Error: zk.fetch: only https:// URLs are allowed"));
    CHECK(contains(log, "OUT Error: zk.fetch: the request body is over 10 MB"));
    CHECK(net.fake.sent.empty());
}

TEST_CASE("fetch: a response over 10 MB fails with LimitError (in the plugin)") {
    FakePlugins fp;
    SharedFake net;
    net.fake.expect("GET", "https://a.example.org/big", FakeHttp::ok(std::string(plugins::kFetchMaxBytes + 1, 'x')));
    fp.add("a", pluginWith(R"(await zk.fetch("https://a.example.org/big"); out("no");)"), {"net:a.example.org"});
    auto deps = fp.deps();
    deps.makeHttp = net.maker();
    std::string log = runOnce(fp, deps, "a");
    CHECK(contains(log, "ERR LimitError"));
    CHECK(contains(log, "limit=responseSize"));
    CHECK_FALSE(contains(log, "OUT no"));
    CHECK_FALSE(contains(log, "Stopped"));  // only that fetch fails
}

TEST_CASE("fetch: a slow host doesn't delay another plugin's handler") {
    FakePlugins fp;
    std::atomic<bool> release{false}, slowReturned{false};
    std::atomic<int> slowStarted{0};
    auto fn = [&](const HttpRequest& r) {
        if (r.url.rfind("https://slow.example.org/", 0) == 0) {
            ++slowStarted;
            while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            slowReturned = true;
            return FakeHttp::ok("slow");
        }
        return FakeHttp::ok("fast");
    };
    fp.add("a", pluginWith(R"(const r = await zk.fetch("https://slow.example.org/"); out(await r.text());)"), {"net:slow.example.org"});
    fp.add("b", pluginWith(R"(
        const [x, y] = await Promise.all([zk.fetch("https://fast.example.org/1"), zk.fetch("https://fast.example.org/2")]);
        out((await x.text()) + (await y.text()));
    )"),
           {"net:fast.example.org"});
    auto deps = fp.deps();
    deps.handlerBudgetMs = 4000;
    deps.makeHttp = [fn] { return std::make_unique<FnHttp>(fn); };
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    host.dispatch(kRun);  // a's handler runs first and waits on the slow host
    CHECK(eventually([&] { return contains(fp.log("b"), "OUT fastfast"); }, 2000));
    CHECK(slowStarted == 1);
    CHECK_FALSE(slowReturned);
    release = true;
    REQUIRE(host.waitIdle(5000));
    CHECK(contains(fp.log("a"), "OUT slow"));
}

TEST_CASE("fetch: a stopped call's fetch is aborted and rejects with the LimitError") {
    FakePlugins fp;
    std::atomic<bool> sawCancel{false};
    auto fn = [&](const HttpRequest& r) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        sawCancel = r.cancel && *r.cancel;
        HttpResponse resp;
        resp.curlCode = 42;
        resp.curlError = "Callback aborted";
        return resp;
    };
    fp.add("a", pluginWith(R"(await zk.fetch("https://slow.example.org/"); out("no");)"), {"net:slow.example.org"});
    auto deps = fp.deps();
    deps.makeHttp = [fn] { return std::make_unique<FnHttp>(fn); };
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    auto t0 = std::chrono::steady_clock::now();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    CHECK(eventually([&] { return sawCancel.load(); }, 2000));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    std::string log = fp.log("a");
    CHECK(contains(log, "ERR LimitError"));
    CHECK(contains(log, "limit=time"));
    CHECK_FALSE(contains(log, "OUT no"));
}

TEST_CASE("fetch: zk.fetch's timeout rejects with LimitError responseTime") {
    FakePlugins fp;
    auto fn = [](const HttpRequest& r) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(r.timeoutMs);
        while (std::chrono::steady_clock::now() < end && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        HttpResponse resp;
        resp.curlCode = 28;
        resp.curlError = "Operation timed out";
        return resp;
    };
    fp.add("a", pluginWith(R"(await zk.fetch("https://slow.example.org/", { timeout: 80 }); out("no");)"), {"net:slow.example.org"});
    auto deps = fp.deps();
    deps.handlerBudgetMs = 2000;
    deps.makeHttp = [fn] { return std::make_unique<FnHttp>(fn); };
    std::string log = runOnce(fp, deps, "a");
    CHECK(contains(log, "ERR LimitError"));
    CHECK(contains(log, "limit=responseTime"));
    CHECK(contains(log, "80 ms"));
}
