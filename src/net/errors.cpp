#include "net/errors.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

namespace zk {

namespace {

constexpr int kCurlCouldntResolveHost = 6;
constexpr int kCurlCouldntConnect = 7;
constexpr int kCurlOperationTimedOut = 28;
constexpr int kCurlPeerFailedVerification = 60;
constexpr int kBlockedByGuard = -1;

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string retryClock(const HttpResponse& r) {
    long seconds = 60;
    if (auto it = r.headers.find("retry-after"); it != r.headers.end()) {
        char* end = nullptr;
        long v = std::strtol(it->second.c_str(), &end, 10);
        if (end != it->second.c_str() && v >= 0) seconds = v;
    }
    std::time_t at = std::time(nullptr) + seconds;
    std::tm tm{};
    localtime_r(&at, &tm);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

UserError make(ErrorKind kind, std::string message, std::string details) {
    return UserError{kind, std::move(message), std::move(details)};
}

}  // namespace

bool isTailnetHost(const std::string& host) {
    if (endsWith(host, ".ts.net")) return true;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4)
        return a == 100 && b >= 64 && b < 128;
    return false;
}

std::string hostOf(const std::string& url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of(":/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

UserError classify(const HttpResponse& r, const NetState& net, const std::string& host, bool hostIsTailnet,
                   const std::string& method) {
    if (r.curlCode == kBlockedByGuard)
        return make(ErrorKind::Blocked, "That change was blocked to protect your library.", r.curlError);

    if (r.curlCode != 0) {
        const std::string details = "curl " + std::to_string(r.curlCode) + ": " + r.curlError;
        if (!net.wifiConnected) return make(ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", details);
        if (hostIsTailnet && !net.tailscaleUp)
            return make(ErrorKind::TailscaleDown, "Tailscale isn't running. Start it from KUAL, then try again.", details);
        switch (r.curlCode) {
            case kCurlCouldntResolveHost: return make(ErrorKind::HostNotFound, "Can't find " + host + ".", details);
            case kCurlCouldntConnect:
            case kCurlOperationTimedOut: return make(ErrorKind::HostNotAnswering, host + " isn't answering.", details);
            case kCurlPeerFailedVerification:
                return make(ErrorKind::Certificate, "Certificate problem with " + host + ".", details);
            default: return make(ErrorKind::Other, "Something went wrong talking to " + host + ".", details);
        }
    }

    const long s = r.status;
    if (s >= 200 && s < 400) return {};
    const std::string details = "HTTP " + std::to_string(s);
    const bool zotero = host == "api.zotero.org";
    if (s == 429 || (s == 503 && r.headers.count("retry-after")))
        return make(ErrorKind::RateLimited, "Zotero is busy — retrying at " + retryClock(r) + ".", details);
    if (zotero) {
        if (s == 401 || s == 403) {
            const bool write = method != "GET" && method != "HEAD";
            return write ? make(ErrorKind::ApiKeyNoWrite, "This API key can't write to your library.", details)
                         : make(ErrorKind::ApiKeyRejected, "Zotero rejected the API key.", details);
        }
        if (s >= 500) return make(ErrorKind::ServerError, "Zotero had a problem (" + std::to_string(s) + "). Try again later.", details);
        return make(ErrorKind::Other, "Zotero refused the request (" + std::to_string(s) + ").", details);
    }
    if (s == 401) return make(ErrorKind::WebDavLogin, "Wrong WebDAV user or password.", details);
    if (s == 404)
        return make(ErrorKind::NotOnServer, "This file isn't on the server yet — sync it from Zotero on your computer.", details);
    if (s == 507) return make(ErrorKind::Other, host + " is out of space.", details);
    return make(ErrorKind::Other, host + " answered with an error (" + std::to_string(s) + ").", details);
}

NetState probeNetState() {
    NetState st;
#ifdef ZK_KINDLE
    st.wifiConnected = false;
    if (FILE* p = popen("lipc-get-prop com.lab126.wifid cmState 2>/dev/null", "r")) {
        char buf[64] = {0};
        if (std::fgets(buf, sizeof buf, p)) st.wifiConnected = std::string(buf).rfind("CONNECTED", 0) == 0;
        pclose(p);
    }
    struct stat sb {};
    st.tailscaleUp = ::stat("/sys/class/net/tailscale0", &sb) == 0;
#endif
    return st;
}

}  // namespace zk
