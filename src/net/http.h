#pragma once
// The HTTP seam: everything that talks to zotero.org or WebDAV goes through `Http`, so tests
// can substitute a scripted fake and the write guard can sit in front of the real client.

#include <map>
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
    int connectTimeoutS = 10;
    int timeoutS = 120;
};

struct HttpResponse {
    long status = 0;
    std::map<std::string, std::string> headers;  // names lower-cased
    std::string body;
    int curlCode = 0;
    std::string curlError;
    double seconds = 0;
};

class Http {
public:
    virtual ~Http() = default;
    virtual HttpResponse send(const HttpRequest&) = 0;
};

class CurlHttp final : public Http {
public:
    explicit CurlHttp(std::string caBundle /* empty = system default */);
    HttpResponse send(const HttpRequest&) override;

private:
    std::string caBundle_;
};

// "METHOD URL" plus headers, with secret header values replaced by ***; never the password.
std::string maskedForLog(const HttpRequest&);

}  // namespace zk
