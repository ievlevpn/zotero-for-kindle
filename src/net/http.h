#pragma once
// The HTTP seam: everything that talks to zotero.org or WebDAV goes through `Http`, so tests
// can substitute a scripted fake and the write guard can sit in front of the real client.

#include <sys/socket.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zk {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string user, password;  // HTTP basic/digest auth when `user` is set
    std::string outFile;         // when set, the response body is streamed to this file
    bool followRedirects = true;  // reads only (writes never follow); false: the 3xx itself is returned
    int connectTimeoutS = 10;
    int timeoutS = 120;
    int timeoutMs = 0;          // > 0: the whole transfer's limit in ms, instead of timeoutS
    size_t maxBodyBytes = 0;    // > 0: the transfer fails (tooLarge) once the body would pass this
    // When set: asked for every address the client is about to connect to (on a fresh connection;
    // no cached one is reused); false refuses it (HttpResponse::addressRefused if none was allowed).
    std::function<bool(const sockaddr*, socklen_t)> allowAddress;
    bool noProxy = false;       // connect directly, whatever the environment's proxy settings
    std::function<void(int64_t done, int64_t total)> onProgress;  // download progress (bytes)
    std::shared_ptr<std::atomic<bool>> cancel;                    // set to abort the transfer
};

struct HttpResponse {
    long status = 0;
    std::map<std::string, std::string> headers;  // names lower-cased; repeated ones joined with ", "
    std::string body;
    std::string url;          // the URL that answered, when the client tracks it (PluginHttp does)
    bool tooLarge = false;    // the body passed HttpRequest::maxBodyBytes; reading stopped there
    bool addressRefused = false;  // HttpRequest::allowAddress refused an address it would have used
    int curlCode = 0;
    std::string curlError;
    double seconds = 0;
};

class Http {
public:
    virtual ~Http() = default;
    virtual HttpResponse send(const HttpRequest&) = 0;
};

// Keeps one curl handle, so consecutive requests to the same host reuse the TLS connection
// (a fresh handshake per request dominated the first sync). Requests are serialised.
class CurlHttp final : public Http {
public:
    explicit CurlHttp(std::string caBundle /* empty = system default */);
    ~CurlHttp() override;
    CurlHttp(const CurlHttp&) = delete;
    CurlHttp& operator=(const CurlHttp&) = delete;
    HttpResponse send(const HttpRequest&) override;

private:
    std::string caBundle_;
    void* handle_ = nullptr;  // CURL*
    std::mutex mutex_;
};

// "METHOD URL" plus headers, with secret header values replaced by ***; never the password.
std::string maskedForLog(const HttpRequest&);

}  // namespace zk
