#pragma once
// Pavel's rule, enforced in one place: the app's writes are additive only. Every request to
// zotero.org or WebDAV passes through this guard, which refuses DELETE and any change to an
// object the app didn't create itself. A refused request never reaches the network.

#include "net/http.h"

#include <functional>
#include <string>

namespace zk {

class WriteGuard final : public Http {
public:
    WriteGuard(Http& inner, std::function<bool(const std::string& key)> createdByUs);
    HttpResponse send(const HttpRequest&) override;

private:
    Http& inner_;
    std::function<bool(const std::string&)> createdByUs_;
};

}  // namespace zk
