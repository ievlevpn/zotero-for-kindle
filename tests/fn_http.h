#pragma once
// An Http made from a function, for wiring fakes together (and blocking on purpose) in tests.

#include "net/http.h"

#include <functional>
#include <utility>

namespace zk::test {

class FnHttp : public Http {
public:
    explicit FnHttp(std::function<HttpResponse(const HttpRequest&)> fn) : fn_(std::move(fn)) {}
    HttpResponse send(const HttpRequest& r) override { return fn_(r); }

private:
    std::function<HttpResponse(const HttpRequest&)> fn_;
};

}  // namespace zk::test
