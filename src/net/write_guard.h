#pragma once
// The app writes annotations and nothing else, enforced in one place. Every request to
// zotero.org or WebDAV passes through this guard: annotations may be created, edited and
// deleted (like in Zotero desktop); every other write is refused and never reaches the network.

#include "net/http.h"

#include <functional>
#include <string>

namespace zk {

class WriteGuard final : public Http {
public:
    WriteGuard(Http& inner, std::function<bool(const std::string& key)> isAnnotation);
    HttpResponse send(const HttpRequest&) override;

private:
    Http& inner_;
    std::function<bool(const std::string&)> isAnnotation_;
};

}  // namespace zk
