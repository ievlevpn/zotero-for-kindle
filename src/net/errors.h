#pragma once
// Turns a failed request into one plain sentence that says what is wrong (spec §7.4), using
// what the device knows about Wi-Fi and Tailscale first, then curl's code, then HTTP status.

#include "net/http.h"

#include <string>

namespace zk {

enum class ErrorKind {
    None, NoWifi, TailscaleDown, HostNotFound, HostNotAnswering, Certificate, WebDavLogin,
    NotOnServer, ApiKeyRejected, ApiKeyNoWrite, RateLimited, ServerError, DiskFull, Checksum,
    Blocked, Other, Cancelled  // Cancelled: the user stopped it (an update download)
};

struct NetState {
    bool wifiConnected = true;
    bool tailscaleUp = true;
};

struct UserError {
    ErrorKind kind = ErrorKind::None;
    std::string message;  // shown to Pavel
    std::string details;  // "HTTP 401" or "curl 28: ..." for the log and a details line
};

// `method` tells a refused read (bad key) from a refused write (key without write access).
UserError classify(const HttpResponse&, const NetState&, const std::string& host, bool hostIsTailnet,
                   const std::string& method = "GET");

// *.ts.net names and Tailscale's 100.64.0.0/10 addresses.
bool isTailnetHost(const std::string& host);

// The host part of a URL: "https://a.b:8443/x" -> "a.b".
std::string hostOf(const std::string& url);

// Kindle: Wi-Fi from lipc (cmState == CONNECTED), Tailscale from /sys/class/net/tailscale0.
// Desktop: both true.
NetState probeNetState();

}  // namespace zk
