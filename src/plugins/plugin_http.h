#pragma once
// Plugins' network access: PluginHttp is the guard every zk.fetch request goes through (never the
// Zotero WriteGuard), and PluginNet the workers that run those requests off the plugin thread.
//
// The guard's rules, checked on the first URL and again on every redirect hop (it follows them
// itself, at most 5): https only; never zotero.org, any *.zotero.org or the WebDAV host, whatever
// the plugin declares; otherwise only hosts the plugin declared (net:<host>, net:*.<domain>) or any
// host with net:* — but a host reached only through net:* must not be on the local network: every
// address actually connected to is checked (isPublicAddress), and no proxy stands in between. A request carries only what the plugin set: no cookies, no app credentials, no
// user name or password; the plugin's Authorization goes no further than its own origin. Responses
// are capped at 10 MB and requests at 30 s (less if the plugin asks).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "net/http.h"
#include "plugins/manifest.h"

namespace zk::plugins {

constexpr size_t kFetchMaxBytes = 10u << 20;  // a response body, and a request body
constexpr int kFetchMaxMs = 30000;            // a whole request, redirects included
constexpr int kFetchMaxRedirects = 5;
constexpr int kFetchMaxPending = 8;                 // a plugin's fetches queued or running
constexpr size_t kFetchMaxPendingBytes = 20u << 20;  // their request bodies together

// False for loopback, private, carrier-grade NAT, link-local, unspecified, multicast and broadcast
// addresses (IPv4-mapped, -compatible and NAT64 IPv6 addresses are judged by their IPv4 part), and
// for anything that isn't a whole IPv4 or IPv6 address.
bool isPublicAddress(const sockaddr*, socklen_t);

// Why a plugin request didn't produce a response.
struct FetchError : std::runtime_error {
    enum class Kind {
        Permission,  // an undeclared host: what() is the detail ("zk.fetch(\"https://h/…\")"),
                     // `detail` the permission to declare ("net:h")
        Limit,       // `detail` is "responseSize" or "responseTime"; what() the message
        Refused,     // never allowed (http:, zotero.org, the WebDAV host, a bad URL, redirect loops)
        Network,     // the host couldn't be reached, or the transfer failed
    };
    FetchError(Kind k, const std::string& message, std::string d = "") : std::runtime_error(message), kind(k), detail(std::move(d)) {}
    Kind kind;
    std::string detail;
};

class PluginHttp {
public:
    // webdavHost: the WebDAV server's host name (a URL is accepted too); "" if none.
    PluginHttp(Http& inner, std::string webdavHost);
    // Sends the plugin's request under the rules above, following redirects itself. Uses from `req`
    // only method, url, headers, body and cancel. Throws FetchError. The response's url is the one
    // that answered. The time limit counts from `started` (when the plugin asked).
    HttpResponse send(const PluginManifest&, HttpRequest req, int timeoutMs = kFetchMaxMs,
                      std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now());
    void setWebdavHost(const std::string&);

private:
    Http& inner_;
    std::string webdav_;
};

// What a request came to: a response, or why not.
struct FetchResult {
    std::optional<HttpResponse> response;
    std::optional<FetchError> error;
};

// A small pool of network workers (threads made as needed, each with its own Http and so its own
// connection), shared by all plugins. A plugin has at most `perPlugin` requests running at once, so
// one plugin's slow hosts never hold up another plugin's requests, and at most kFetchMaxPending
// queued or running, with kFetchMaxPendingBytes of request bodies (their memory is the app's, not
// the plugin's runtime's).
class PluginNet {
public:
    struct Job {
        std::string pluginId;
        PluginManifest manifest;
        HttpRequest request;  // cancel: set it to abort (a queued job is then dropped, `done` not called)
        int timeoutMs = kFetchMaxMs;
        std::function<void(FetchResult)> done;  // on a worker thread
    };

    // webdavHost is asked before every request: the app may change its WebDAV server at any time.
    PluginNet(std::function<std::unique_ptr<Http>()> makeHttp, std::function<std::string()> webdavHost, int maxWorkers = 6,
              int perPlugin = 2);
    ~PluginNet();  // shutdown()
    PluginNet(const PluginNet&) = delete;
    PluginNet& operator=(const PluginNet&) = delete;

    // The request's time counts from now (time queued included). A LimitError ("fetches") instead,
    // job not taken, if the plugin has too many fetches or body bytes under way.
    std::optional<FetchError> submit(Job);
    // Releases queued jobs whose cancel flag is set (the host calls it after cancelling work).
    void sweep();
    // Drops queued jobs, aborts running ones and joins the workers.
    void shutdown();

    struct Pending {
        int jobs = 0;      // queued or running
        size_t bytes = 0;  // their request bodies
    };
    Pending pending(const std::string& pluginId);

private:
    struct Queued {
        Job job;
        std::chrono::steady_clock::time_point started;
    };
    void work();
    bool pickable() const;
    void release(const std::string& pluginId, size_t bytes);  // under mu_
    void sweepLocked();

    std::function<std::unique_ptr<Http>()> makeHttp_;
    std::function<std::string()> webdav_;
    const int maxWorkers_, perPlugin_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Queued> queue_;
    std::map<std::string, int> active_;  // running jobs per plugin
    std::map<std::string, Pending> pending_;  // queued and running, per plugin
    std::set<std::shared_ptr<std::atomic<bool>>> running_;  // their cancel flags
    std::vector<std::thread> threads_;
    int idle_ = 0;
    bool stopping_ = false;
};

}  // namespace zk::plugins
