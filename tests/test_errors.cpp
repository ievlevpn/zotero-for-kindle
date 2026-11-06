#include <doctest/doctest.h>

#include "net/errors.h"

using namespace zk;

namespace {

HttpResponse curlFail(int code, std::string msg = "x") {
    HttpResponse r;
    r.curlCode = code;
    r.curlError = std::move(msg);
    return r;
}

HttpResponse http(long status, std::map<std::string, std::string> headers = {}) {
    HttpResponse r;
    r.status = status;
    r.headers = std::move(headers);
    return r;
}

const std::string dav = "brighton.example.ts.net";
const std::string api = "api.zotero.org";
const NetState allUp{};

}  // namespace

TEST_CASE("classify: Wi-Fi off wins over the curl code") {
    UserError e = classify(curlFail(6), NetState{false, true}, api, false);
    CHECK(e.kind == ErrorKind::NoWifi);
    CHECK(e.message == "No Wi-Fi. Connect to a network and try again.");
    CHECK(e.details == "curl 6: x");
}

TEST_CASE("classify: tailnet host with Tailscale down") {
    UserError e = classify(curlFail(28), NetState{true, false}, dav, true);
    CHECK(e.kind == ErrorKind::TailscaleDown);
    CHECK(e.message == "Tailscale isn't running. Start it from KUAL, then try again.");
}

TEST_CASE("classify: curl codes for a reachable network") {
    CHECK(classify(curlFail(6), allUp, "nowhere.example", false).message == "Can't find nowhere.example.");
    CHECK(classify(curlFail(6), allUp, "nowhere.example", false).kind == ErrorKind::HostNotFound);
    CHECK(classify(curlFail(28), allUp, dav, true).kind == ErrorKind::HostNotAnswering);
    CHECK(classify(curlFail(28), allUp, dav, true).message == dav + " isn't answering.");
    CHECK(classify(curlFail(7), allUp, dav, true).kind == ErrorKind::HostNotAnswering);
    CHECK(classify(curlFail(60), allUp, dav, true).kind == ErrorKind::Certificate);
    CHECK(classify(curlFail(60), allUp, dav, true).message == "Certificate problem with " + dav + ".");
}

TEST_CASE("classify: WebDAV HTTP statuses") {
    UserError login = classify(http(401), allUp, dav, true);
    CHECK(login.kind == ErrorKind::WebDavLogin);
    CHECK(login.message == "Wrong WebDAV user or password.");
    CHECK(login.details == "HTTP 401");
    UserError missing = classify(http(404), allUp, dav, true);
    CHECK(missing.kind == ErrorKind::NotOnServer);
    CHECK(missing.message == "This file isn't on the server yet — sync it from Zotero on your computer.");
}

TEST_CASE("classify: zotero.org rejects the key on reads and on writes") {
    CHECK(classify(http(403), allUp, api, false, "GET").kind == ErrorKind::ApiKeyRejected);
    CHECK(classify(http(403), allUp, api, false, "GET").message == "Zotero rejected the API key.");
    CHECK(classify(http(403), allUp, api, false, "POST").kind == ErrorKind::ApiKeyNoWrite);
    CHECK(classify(http(403), allUp, api, false, "POST").message == "This API key can't write to your library.");
}

TEST_CASE("classify: rate limiting and server errors") {
    UserError busy = classify(http(429, {{"retry-after", "60"}}), allUp, api, false);
    CHECK(busy.kind == ErrorKind::RateLimited);
    CHECK(busy.message.find("Zotero is busy — retrying at ") == 0);
    CHECK(busy.message.size() == std::string("Zotero is busy — retrying at HH:MM.").size());
    CHECK(classify(http(503, {{"retry-after", "5"}}), allUp, api, false).kind == ErrorKind::RateLimited);
    UserError broken = classify(http(500), allUp, api, false);
    CHECK(broken.kind == ErrorKind::ServerError);
    CHECK(broken.message == "Zotero had a problem (500). Try again later.");
}

TEST_CASE("classify: success and write-guard blocks") {
    CHECK(classify(http(200), allUp, api, false).kind == ErrorKind::None);
    CHECK(classify(http(204), allUp, api, false).kind == ErrorKind::None);
    UserError blocked = classify(curlFail(-1, "blocked by write guard: DELETE is never sent"), allUp, api, false);
    CHECK(blocked.kind == ErrorKind::Blocked);
    CHECK(blocked.details == "blocked by write guard: DELETE is never sent");
}

TEST_CASE("isTailnetHost recognises ts.net names and CGNAT addresses") {
    CHECK(isTailnetHost("brighton.example.ts.net"));
    CHECK(isTailnetHost("100.81.5.116"));
    CHECK_FALSE(isTailnetHost("100.128.0.1"));
    CHECK_FALSE(isTailnetHost("api.zotero.org"));
    CHECK_FALSE(isTailnetHost("192.168.1.10"));
}

TEST_CASE("classify: Tailscale is only suspected for a tailnet WebDAV host") {
    // Most people's WebDAV is a normal internet or LAN server; Tailscale being off is irrelevant.
    const NetState noTailscale{true, false};
    UserError e = classify(curlFail(28), noTailscale, "dav.example.org", false);
    CHECK(e.kind == ErrorKind::HostNotAnswering);
    CHECK(e.message == "dav.example.org isn't answering.");
    CHECK(classify(curlFail(6), noTailscale, "dav.example.org", false).kind == ErrorKind::HostNotFound);
    CHECK(classify(http(401), noTailscale, "dav.example.org", false).kind == ErrorKind::WebDavLogin);
    CHECK_FALSE(isTailnetHost("dav.example.org"));
    CHECK_FALSE(isTailnetHost("192.168.1.20"));
}
