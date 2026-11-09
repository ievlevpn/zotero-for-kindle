#include "plugins/plugin_http.h"

#include <algorithm>
#include <cctype>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace zk::plugins {

namespace {

bool publicV4(uint32_t a) {  // host byte order
    auto in = [a](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
    return !(in(0x00000000, 8) ||   // 0/8: "this network", unspecified
             in(0x0A000000, 8) ||   // 10/8
             in(0x64400000, 10) ||  // 100.64/10: carrier-grade NAT
             in(0x7F000000, 8) ||   // 127/8: loopback
             in(0xA9FE0000, 16) ||  // 169.254/16: link-local (cloud metadata)
             in(0xAC100000, 12) ||  // 172.16/12
             in(0xC0A80000, 16) ||  // 192.168/16
             in(0xE0000000, 3));    // 224/4 multicast, 240/4 reserved, broadcast
}

}  // namespace

bool isPublicAddress(const sockaddr* sa, socklen_t len) {
    if (!sa || len < static_cast<socklen_t>(sizeof(sa_family_t))) return false;
    if (sa->sa_family == AF_INET) {
        if (len < static_cast<socklen_t>(sizeof(sockaddr_in))) return false;
        sockaddr_in v4;
        std::memcpy(&v4, sa, sizeof v4);
        return publicV4(ntohl(v4.sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        if (len < static_cast<socklen_t>(sizeof(sockaddr_in6))) return false;
        sockaddr_in6 v6;
        std::memcpy(&v6, sa, sizeof v6);
        const unsigned char* b = v6.sin6_addr.s6_addr;
        auto v4At12 = [b] { return (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) | (uint32_t(b[14]) << 8) | b[15]; };
        auto zeros = [b](int from, int to) {
            for (int i = from; i < to; ++i)
                if (b[i]) return false;
            return true;
        };
        if (zeros(0, 10) && b[10] == 0xFF && b[11] == 0xFF) return publicV4(v4At12());  // ::ffff:a.b.c.d
        if (zeros(0, 12)) return publicV4(v4At12());  // ::a.b.c.d (also :: and ::1)
        static const unsigned char kNat64[12] = {0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0};
        if (std::memcmp(b, kNat64, 12) == 0) return publicV4(v4At12());  // 64:ff9b::/96
        if ((b[0] & 0xFE) == 0xFC) return false;                  // fc00::/7 unique local
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false;  // fe80::/10 link-local
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) return false;  // fec0::/10 site-local (old)
        if (b[0] == 0xFF) return false;                           // multicast
        return true;
    }
    return false;
}

namespace {

using Clock = std::chrono::steady_clock;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool isToken(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos)) return false;
    return true;
}

// A URL as the guard sees it: scheme and host lower-cased, the host without a final dot, the
// default port dropped, the fragment gone.
struct Url {
    std::string scheme, host, port, rest;  // rest: path and query, starting with "/"
    bool userinfo = false;

    std::string origin() const { return scheme + "://" + host + (port.empty() ? "" : ":" + port); }
    std::string str() const { return origin() + rest; }
    std::string shown() const { return origin() + "/…"; }
    bool sameOrigin(const Url& o) const { return scheme == o.scheme && host == o.host && port == o.port; }
};

std::optional<Url> parseUrl(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return std::nullopt;
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0) return std::nullopt;
    Url u;
    u.scheme = lower(s.substr(0, colon));
    if (!std::isalpha(static_cast<unsigned char>(u.scheme[0]))) return std::nullopt;
    for (unsigned char c : u.scheme)
        if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return std::nullopt;
    if (s.compare(colon + 1, 2, "//") != 0) return std::nullopt;
    const size_t authStart = colon + 3;
    size_t authEnd = s.find_first_of("/?#", authStart);
    if (authEnd == std::string::npos) authEnd = s.size();
    std::string auth = s.substr(authStart, authEnd - authStart);
    if (size_t at = auth.rfind('@'); at != std::string::npos) {
        u.userinfo = true;
        auth = auth.substr(at + 1);
    }
    std::string host, port;
    if (!auth.empty() && auth[0] == '[') {
        size_t close = auth.find(']');
        if (close == std::string::npos) return std::nullopt;
        host = lower(auth.substr(0, close + 1));
        for (size_t i = 1; i < close; ++i) {
            const unsigned char c = static_cast<unsigned char>(host[i]);
            if (!(std::isxdigit(c) || c == ':' || c == '.')) return std::nullopt;
        }
        if (close + 1 < auth.size()) {
            if (auth[close + 1] != ':') return std::nullopt;
            port = auth.substr(close + 2);
        }
    } else {
        size_t pc = auth.rfind(':');
        host = lower(auth.substr(0, pc));
        if (pc != std::string::npos) port = auth.substr(pc + 1);
        if (!host.empty() && host.back() == '.') host.pop_back();
        if (host.empty() || host.front() == '.' || host.find("..") != std::string::npos) return std::nullopt;
        for (unsigned char c : host)  // no percent-escapes, no Unicode (punycode is fine): one spelling per host
            if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return std::nullopt;
    }
    if (host.empty() || host == "[]") return std::nullopt;
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); }))
            return std::nullopt;
        const long n = std::stol(port);
        if (n < 1 || n > 65535) return std::nullopt;
        port = std::to_string(n);
        if ((u.scheme == "https" && n == 443) || (u.scheme == "http" && n == 80)) port.clear();
    }
    u.host = host;
    u.port = port;
    std::string rest = s.substr(authEnd);
    if (size_t hash = rest.find('#'); hash != std::string::npos) rest.erase(hash);
    if (rest.empty() || rest[0] == '?') rest = "/" + rest;
    // What servers send in Location may be raw: escape spaces and non-ASCII bytes, as browsers do.
    for (unsigned char c : rest) {
        if (c == ' ' || c >= 0x80) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            u.rest += buf;
        } else {
            u.rest += static_cast<char>(c);
        }
    }
    return u;
}

// A Location header's target, resolved against the URL that sent it.
std::optional<Url> resolve(const Url& base, std::string loc) {
    while (!loc.empty() && (loc.front() == ' ' || loc.front() == '\t')) loc.erase(0, 1);
    while (!loc.empty() && (loc.back() == ' ' || loc.back() == '\t')) loc.pop_back();
    size_t colon = loc.find(':');
    size_t firstSep = loc.find_first_of("/?#");
    if (colon != std::string::npos && colon > 0 && (firstSep == std::string::npos || colon < firstSep) &&
        std::isalpha(static_cast<unsigned char>(loc[0])))
        return parseUrl(loc);
    if (loc.rfind("//", 0) == 0) return parseUrl(base.scheme + ":" + loc);
    std::string path = base.rest.substr(0, base.rest.find('?'));
    if (loc.empty() || loc[0] == '#') return parseUrl(base.str());
    if (loc[0] == '/') return parseUrl(base.origin() + loc);
    if (loc[0] == '?') return parseUrl(base.origin() + path + loc);
    return parseUrl(base.origin() + path.substr(0, path.rfind('/') + 1) + loc);
}

std::string normalisedHost(const std::string& configured) {
    if (configured.empty()) return "";
    if (configured.find("://") != std::string::npos) {
        auto u = parseUrl(configured);
        return u ? u->host : lower(configured);
    }
    if (auto u = parseUrl("https://" + configured)) return u->host;
    return lower(configured);
}

bool isZotero(const std::string& host) {
    return host == "zotero.org" || (host.size() > 11 && host.compare(host.size() - 11, 11, ".zotero.org") == 0);
}

bool isIpLiteral(const std::string& host) {
    return host[0] == '[' || std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; });
}

std::string clip(const std::string& s) { return s.size() > 200 ? s.substr(0, 200) + "…" : s; }

std::string duration(int ms) {
    if (ms % 1000 == 0) return std::to_string(ms / 1000) + " s";
    return std::to_string(ms) + " ms";
}

// Declared by name (net:<host> or net:*.<domain>), not just covered by net:*.
bool declared(const PluginManifest& m, const std::string& host) {
    PluginManifest named;
    named.hosts = m.hosts;
    named.permissions.insert(Permission::Net);
    return hostAllowed(named, host);
}

// The rules for one URL, the first (`from` null) or a redirect's target. anyOnly: allowed only by
// net:* (its addresses must then be public).
Url check(const PluginManifest& m, const std::optional<Url>& u, const std::string& raw, const Url* from,
          const std::string& webdav, bool& anyOnly) {
    const std::string at = from ? "zk.fetch(\"" + from->shown() + "\"): the redirect to " : "zk.fetch: ";
    using K = FetchError::Kind;
    if (!u) throw FetchError(K::Refused, at + "\"" + clip(raw) + "\" isn't a valid URL");
    if (u->scheme != "https")
        throw FetchError(K::Refused, from ? at + u->shown() + " is refused (only https:// is allowed)"
                                          : "zk.fetch: only https:// URLs are allowed (\"" + clip(raw) + "\")");
    if (isZotero(u->host) || (!webdav.empty() && u->host == webdav))
        throw FetchError(K::Refused, (from ? at + u->shown() + " is refused: p" : "zk.fetch: p") + "lugins can't connect to " +
                                         u->host + " (zotero.org and the WebDAV server are the app's own)");
    if (u->userinfo)
        throw FetchError(K::Refused, at + (from ? u->shown() + " is refused: " : "") +
                                         "a user name or password in the URL isn't allowed (set an Authorization header)");
    anyOnly = !declared(m, u->host);
    if (!hostAllowed(m, u->host)) {
        const std::string perm = isIpLiteral(u->host) ? "net:*" : "net:" + u->host;
        throw FetchError(K::Permission, from ? at + u->shown() : "zk.fetch(\"" + u->shown() + "\")", perm);
    }
    return *u;
}

bool dropped(const std::string& lowerName) {
    static const char* const kDropped[] = {"cookie",  "cookie2", "host",    "content-length", "transfer-encoding",
                                           "connection", "keep-alive", "upgrade", "te", "trailer",
                                           "proxy-connection", "proxy-authorization", "expect"};
    for (const char* d : kDropped)
        if (lowerName == d) return true;
    return false;
}

bool isRedirect(long status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

}  // namespace

PluginHttp::PluginHttp(Http& inner, std::string webdavHost) : inner_(inner), webdav_(normalisedHost(webdavHost)) {}

void PluginHttp::setWebdavHost(const std::string& host) { webdav_ = normalisedHost(host); }

HttpResponse PluginHttp::send(const PluginManifest& m, HttpRequest in, int timeoutMs, Clock::time_point started) {
    using K = FetchError::Kind;
    timeoutMs = std::clamp(timeoutMs, 1, kFetchMaxMs);
    const auto deadline = started + std::chrono::milliseconds(timeoutMs);
    bool anyOnly = false;
    Url u = check(m, parseUrl(in.url), in.url, nullptr, webdav_, anyOnly);
    const std::string first = u.shown();

    std::string method = in.method;
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return std::toupper(c); });
    static const std::set<std::string> kMethods = {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE"};
    if (!kMethods.count(method)) throw FetchError(K::Refused, "zk.fetch: the method \"" + clip(in.method) + "\" isn't allowed");
    std::string body = std::move(in.body);
    if (!body.empty() && (method == "GET" || method == "HEAD"))
        throw FetchError(K::Refused, "zk.fetch: a " + method + " request can't have a body");
    if (body.size() > kFetchMaxBytes) throw FetchError(K::Refused, "zk.fetch: the request body is over 10 MB");

    // Only the plugin's own headers, minus cookies and what only the HTTP layer may set.
    std::vector<std::pair<std::string, std::string>> headers;
    for (auto& [k, v] : in.headers) {
        if (!isToken(k) || v.find_first_of(std::string("\r\n\0", 3)) != std::string::npos || dropped(lower(k))) continue;
        headers.emplace_back(k, v);
    }
    auto removeHeaders = [&](std::initializer_list<const char*> names) {
        headers.erase(std::remove_if(headers.begin(), headers.end(),
                                     [&](const auto& h) {
                                         const std::string l = lower(h.first);
                                         return std::any_of(names.begin(), names.end(), [&](const char* n) { return l == n; });
                                     }),
                      headers.end());
    };

    for (int redirects = 0;; ++redirects) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) throw FetchError(K::Limit, "zk.fetch(\"" + first + "\") took longer than " + duration(timeoutMs), "responseTime");

        HttpRequest r;  // fresh: nothing of the caller's but what is copied here
        r.method = method;
        r.url = u.str();
        r.headers = headers;
        r.body = body;
        r.followRedirects = false;
        r.timeoutMs = static_cast<int>(left);
        r.maxBodyBytes = kFetchMaxBytes;
        r.cancel = in.cancel;
        r.noProxy = true;  // the address checked is the peer's
        if (anyOnly) r.allowAddress = [](const sockaddr* a, socklen_t n) { return isPublicAddress(a, n); };
        HttpResponse resp = inner_.send(r);

        if (in.cancel && *in.cancel) throw FetchError(K::Network, "zk.fetch(\"" + first + "\") was cancelled");
        if (resp.addressRefused && resp.curlCode != 0)
            throw FetchError(K::Refused, "zk.fetch(\"" + u.shown() + "\"): " + u.host +
                                             " is on your local network; plugins with net:* can't reach it — declare it by name (\"net:" +
                                             u.host + "\") in plugin.json");
        if (resp.tooLarge || resp.body.size() > kFetchMaxBytes)
            throw FetchError(K::Limit, "zk.fetch(\"" + first + "\"): the response is over 10 MB", "responseSize");
        if (resp.curlCode == 28 /* CURLE_OPERATION_TIMEDOUT */ || (resp.curlCode != 0 && Clock::now() >= deadline))
            throw FetchError(K::Limit, "zk.fetch(\"" + first + "\") took longer than " + duration(timeoutMs), "responseTime");
        if (resp.curlCode != 0)
            throw FetchError(K::Network, "zk.fetch(\"" + u.shown() + "\") failed: " +
                                             (resp.curlError.empty() ? "network error " + std::to_string(resp.curlCode) : resp.curlError));

        auto loc = resp.headers.find("location");
        if (!isRedirect(resp.status) || loc == resp.headers.end() || loc->second.empty()) {
            resp.url = u.str();
            return resp;
        }
        if (redirects == kFetchMaxRedirects)
            throw FetchError(K::Refused, "zk.fetch(\"" + first + "\"): more than " + std::to_string(kFetchMaxRedirects) + " redirects");
        Url next = check(m, resolve(u, loc->second), loc->second, &u, webdav_, anyOnly);
        // As browsers do: 303 (and 301/302 after a POST) turn into a GET without the body.
        if ((resp.status == 303 && method != "HEAD") || ((resp.status == 301 || resp.status == 302) && method == "POST")) {
            if (method != "HEAD") method = "GET";
            body.clear();
            removeHeaders({"content-type", "content-encoding", "content-language", "content-location"});
        }
        if (!next.sameOrigin(u)) removeHeaders({"authorization"});  // the plugin's key stays with its host
        u = std::move(next);
    }
}

// ---- The workers ----------------------------------------------------------------------------------

PluginNet::PluginNet(std::function<std::unique_ptr<Http>()> makeHttp, std::function<std::string()> webdavHost, int maxWorkers,
                     int perPlugin)
    : makeHttp_(std::move(makeHttp)), webdav_(std::move(webdavHost)), maxWorkers_(maxWorkers), perPlugin_(perPlugin) {}

PluginNet::~PluginNet() { shutdown(); }

void PluginNet::release(const std::string& id, size_t bytes) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    it->second.jobs -= 1;
    it->second.bytes -= std::min(bytes, it->second.bytes);
    if (it->second.jobs <= 0) pending_.erase(it);
}

void PluginNet::sweepLocked() {
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (*it->job.request.cancel) {
            release(it->job.pluginId, it->job.request.body.size());
            it = queue_.erase(it);  // and its body with it
        } else {
            ++it;
        }
    }
}

void PluginNet::sweep() {
    std::lock_guard<std::mutex> lock(mu_);
    sweepLocked();
    cv_.notify_all();
}

PluginNet::Pending PluginNet::pending(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pending_.find(id);
    return it == pending_.end() ? Pending{} : it->second;
}

std::optional<FetchError> PluginNet::submit(Job job) {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) return FetchError(FetchError::Kind::Network, "zk.fetch: the app is closing");
    sweepLocked();
    Pending& mine = pending_[job.pluginId];
    const size_t bytes = job.request.body.size();
    std::optional<FetchError> refused;
    if (mine.jobs >= kFetchMaxPending)
        refused = FetchError(FetchError::Kind::Limit,
                             "zk.fetch: this plugin already has " + std::to_string(kFetchMaxPending) +
                                 " fetches under way; await some before starting more",
                             "fetches");
    else if (mine.bytes + bytes > kFetchMaxPendingBytes)
        refused = FetchError(FetchError::Kind::Limit,
                             "zk.fetch: the request bodies of this plugin's fetches under way would pass 20 MB; await some "
                             "before starting more",
                             "fetches");
    if (refused) {
        if (mine.jobs == 0) pending_.erase(job.pluginId);
        return refused;
    }
    mine.jobs += 1;
    mine.bytes += bytes;
    if (!job.request.cancel) job.request.cancel = std::make_shared<std::atomic<bool>>(false);
    queue_.push_back({std::move(job), Clock::now()});
    // A thread for every job no waiting worker can take (bounded): a busy worker is never waited for.
    if (static_cast<int>(queue_.size()) > idle_ && static_cast<int>(threads_.size()) < maxWorkers_)
        threads_.emplace_back([this] { work(); });
    cv_.notify_all();
    return std::nullopt;
}

bool PluginNet::pickable() const {
    for (const auto& q : queue_) {
        if (*q.job.request.cancel) return true;  // to drop it
        auto a = active_.find(q.job.pluginId);
        if (a == active_.end() || a->second < perPlugin_) return true;
    }
    return false;
}

void PluginNet::work() {
    std::unique_ptr<Http> http;
    std::unique_ptr<PluginHttp> guard;
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        ++idle_;
        cv_.wait(lock, [&] { return stopping_ || pickable(); });
        --idle_;
        if (stopping_) return;
        sweepLocked();
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Queued& q) {
            auto a = active_.find(q.job.pluginId);
            return a == active_.end() || a->second < perPlugin_;
        });
        if (it == queue_.end()) continue;
        Queued q = std::move(*it);
        queue_.erase(it);
        const std::string id = q.job.pluginId;
        const size_t bytes = q.job.request.body.size();
        auto cancel = q.job.request.cancel;
        ++active_[id];
        running_.insert(cancel);
        lock.unlock();

        FetchResult result;
        try {
            if (!http) {
                http = makeHttp_();
                if (!http) throw std::runtime_error("the network isn't available");
                guard = std::make_unique<PluginHttp>(*http, "");
            }
            guard->setWebdavHost(webdav_ ? webdav_() : std::string());  // as configured now
            result.response = guard->send(q.job.manifest, std::move(q.job.request), q.job.timeoutMs, q.started);
        } catch (const FetchError& e) {
            result.error = e;
        } catch (const std::exception& e) {
            result.error = FetchError(FetchError::Kind::Network, std::string("zk.fetch failed: ") + e.what());
        }

        lock.lock();
        if (--active_[id] <= 0) active_.erase(id);
        running_.erase(cancel);
        release(id, bytes);  // before `done`: the plugin's next fetch, started from its settle, has room
        cv_.notify_all();
        lock.unlock();
        try {
            if (!*cancel) q.job.done(std::move(result));
        } catch (...) {  // nothing to tell; the promise is settled by its call's end
        }
        q = Queued{};  // the response and body go now, not at the next job
        lock.lock();
    }
}

void PluginNet::shutdown() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
        for (const auto& c : running_) *c = true;  // aborts the transfers (curl checks it while it waits)
        queue_.clear();
        pending_.clear();
        threads.swap(threads_);
    }
    cv_.notify_all();
    for (auto& t : threads) t.join();
}

}  // namespace zk::plugins
