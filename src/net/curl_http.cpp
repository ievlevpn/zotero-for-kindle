#include "net/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace zk {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

struct Sink {
    HttpResponse* resp;
    FILE* file;
};

size_t onBody(char* data, size_t size, size_t n, void* user) {
    auto* sink = static_cast<Sink*>(user);
    const size_t len = size * n;
    if (sink->file) return std::fwrite(data, 1, len, sink->file);
    sink->resp->body.append(data, len);
    return len;
}

size_t onHeader(char* data, size_t size, size_t n, void* user) {
    auto* resp = static_cast<HttpResponse*>(user);
    const size_t len = size * n;
    std::string line(data, len);
    if (line.rfind("HTTP/", 0) == 0) {
        resp->headers.clear();  // a new response (after a redirect or 100-continue)
    } else if (size_t colon = line.find(':'); colon != std::string::npos) {
        resp->headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return len;
}

void globalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

}  // namespace

CurlHttp::CurlHttp(std::string caBundle) : caBundle_(std::move(caBundle)) { globalInit(); }

HttpResponse CurlHttp::send(const HttpRequest& req) {
    HttpResponse resp;
    CURL* h = curl_easy_init();
    if (!h) {
        resp.curlCode = CURLE_FAILED_INIT;
        resp.curlError = "curl_easy_init failed";
        return resp;
    }
    FILE* out = nullptr;
    if (!req.outFile.empty()) {
        out = std::fopen(req.outFile.c_str(), "wb");
        if (!out) {
            curl_easy_cleanup(h);
            resp.curlCode = CURLE_WRITE_ERROR;
            resp.curlError = "cannot open " + req.outFile;
            return resp;
        }
    }
    Sink sink{&resp, out};
    char errbuf[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, static_cast<long>(req.connectTimeoutS));
    curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(req.timeoutS));
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &resp);
    if (!caBundle_.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, caBundle_.c_str());

    if (req.method == "HEAD") {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (req.method != "GET") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }
    if (!req.body.empty() || req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
    }
    if (!req.user.empty()) {
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
        curl_easy_setopt(h, CURLOPT_USERNAME, req.user.c_str());
        curl_easy_setopt(h, CURLOPT_PASSWORD, req.password.c_str());
    }
    curl_slist* headers = nullptr;
    for (const auto& [k, v] : req.headers) headers = curl_slist_append(headers, (k + ": " + v).c_str());
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

    const auto t0 = std::chrono::steady_clock::now();
    CURLcode rc = curl_easy_perform(h);
    resp.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    resp.curlCode = rc;
    if (rc != CURLE_OK) resp.curlError = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    if (out) std::fclose(out);
    return resp;
}

std::string maskedForLog(const HttpRequest& req) {
    std::string s = req.method + " " + req.url;
    for (const auto& [k, v] : req.headers) {
        const std::string n = lower(k);
        const bool secret = n == "zotero-api-key" || n == "authorization";
        s += "\n  " + k + ": " + (secret ? "***" : v);
    }
    if (!req.user.empty()) s += "\n  (auth user " + req.user + ", password ***)";
    return s;
}

}  // namespace zk
