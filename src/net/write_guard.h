#pragma once
// The app writes annotations, reading positions and the read tag, and nothing else, enforced in
// one place. Every request to zotero.org or WebDAV passes through this guard: annotations may be
// created, edited and deleted (like in Zotero desktop), the reading-position settings of the
// library's own files (lastPageIndex_u_<attachmentKey>) may be set, and an item's tags may gain or
// lose one tag (mark as read), against the item's version and with no other field; every other
// write is refused and never reaches the network.

#include "net/http.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zk {

class WriteGuard final : public Http {
public:
    using ItemTags = std::function<std::optional<std::vector<std::string>>(const std::string& key)>;
    // isAttachment: a file of the library (its reading position may be set); null = no settings.
    // itemTags: the tag names of a regular item of the library, nullopt for anything else (its
    // tags may change by one tag); null = no item writes.
    WriteGuard(Http& inner, std::function<bool(const std::string& key)> isAnnotation,
               std::function<bool(const std::string& key)> isAttachment = nullptr, ItemTags itemTags = nullptr);
    HttpResponse send(const HttpRequest&) override;

private:
    Http& inner_;
    std::function<bool(const std::string&)> isAnnotation_, isAttachment_;
    ItemTags itemTags_;
};

}  // namespace zk
