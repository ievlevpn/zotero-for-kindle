#pragma once
// A scripted Http for tests: queued (method, url-prefix) -> response expectations, consumed
// in order of first match; every request is recorded in `sent`.

#include "net/http.h"

#include <deque>
#include <string>
#include <vector>

namespace zk::test {

class FakeHttp : public Http {
public:
    struct Expect {
        std::string method;
        std::string urlPrefix;
        HttpResponse response;
    };

    void expect(std::string method, std::string urlPrefix, HttpResponse response) {
        queue.push_back({std::move(method), std::move(urlPrefix), std::move(response)});
    }

    // Convenience: a 200 with a body and optional headers (lower-case names).
    static HttpResponse ok(std::string body, std::map<std::string, std::string> headers = {}) {
        HttpResponse r;
        r.status = 200;
        r.body = std::move(body);
        r.headers = std::move(headers);
        return r;
    }

    static HttpResponse status(long code, std::map<std::string, std::string> headers = {}) {
        HttpResponse r;
        r.status = code;
        r.headers = std::move(headers);
        return r;
    }

    HttpResponse send(const HttpRequest& req) override {
        sent.push_back(req);
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->method == req.method && req.url.rfind(it->urlPrefix, 0) == 0) {
                HttpResponse r = std::move(it->response);
                queue.erase(it);
                return r;
            }
        }
        HttpResponse r;
        r.status = 599;
        r.curlError = "unexpected request";
        return r;
    }

    std::deque<Expect> queue;
    std::vector<HttpRequest> sent;
};

}  // namespace zk::test
