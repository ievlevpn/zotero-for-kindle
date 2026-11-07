#pragma once
// The app writes annotations and reading positions and nothing else, enforced in one place.
// Every request to zotero.org or WebDAV passes through this guard: annotations may be created,
// edited and deleted (like in Zotero desktop), and the reading-position settings of the library's
// own files (lastPageIndex_u_<attachmentKey>) may be set; every other write is refused and never
// reaches the network.

#include "net/http.h"

#include <functional>
#include <string>

namespace zk {

class WriteGuard final : public Http {
public:
    // isAttachment: a file of the library (its reading position may be set); null = no settings.
    WriteGuard(Http& inner, std::function<bool(const std::string& key)> isAnnotation,
               std::function<bool(const std::string& key)> isAttachment = nullptr);
    HttpResponse send(const HttpRequest&) override;

private:
    Http& inner_;
    std::function<bool(const std::string&)> isAnnotation_, isAttachment_;
};

}  // namespace zk
