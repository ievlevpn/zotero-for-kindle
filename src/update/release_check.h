#pragma once
// Is there a newer release? One GET of GitHub's "latest release" (manual checks only: the
// unauthenticated limit of 60 an hour is plenty).

#include "net/errors.h"
#include "net/http.h"
#include "update/version.h"

#include <cstdint>
#include <functional>
#include <string>

namespace zk {

extern const char* const kDefaultFeed;  // api.github.com/repos/ievlevpn/zotero-for-kindle/releases/latest

struct ReleaseInfo {
    Version version;
    std::string notes;  // plain text (plainNotes of the release's body); not signed, display only
    int64_t zipSize = 0;
    std::string zipUrl, manifestUrl, sigUrl;
};

struct CheckResult {
    // NotInstallable: newer, but without update.json / its signature / the zip (made by hand).
    enum class Kind { UpToDate, Available, NotInstallable, Failed } kind = Kind::Failed;
    ReleaseInfo release;
    UserError error;
};

CheckResult checkLatest(Http&, const std::string& feedUrl, Version current,
                        std::function<NetState()> probe = probeNetState);

// Markdown release notes as plain text for the Kindle: no #, **, `, links as their text, "- " as "• ".
std::string plainNotes(const std::string& markdown);

}  // namespace zk
