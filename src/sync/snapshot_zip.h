#pragma once
// Web snapshots (Zotero's saved pages, text/html): Zotero storage and WebDAV keep one as a zip of
// the page and its files. It is unpacked whole into files/<key>/, the page's MD5 checked as for any
// file. Every entry is checked before anything is written: a relative path that stays inside the
// folder, no symlinks, no encryption, and caps on the entries and on the unpacked size.

#include "net/errors.h"
#include "store/records.h"

#include <cstdint>
#include <functional>
#include <string>

namespace zk {

struct SnapshotLimits {
    uint64_t maxBytes = 256ull << 20;  // the unpacked tree
    unsigned maxEntries = 5000;
};

// The zip at `zipPath` unpacked into destDir/<key>/ (what was there goes), the page named
// localFileName(a) checked against a.md5 and given `mtimeMs` (0: left as unpacked). `freeBytes`
// (asked before, and after a failed write) must cover the tree. On failure *err says why and
// nothing is left behind but the zip (the caller's). Unsafe or oversized zips are refused as damaged.
bool installSnapshot(const AttachmentRecord& a, const std::string& zipPath, const std::string& destDir, int64_t mtimeMs,
                     const std::function<uint64_t()>& freeBytes, UserError* err, const SnapshotLimits& limits = {});

// The file starts as a zip does ("PK\3\4").
bool startsAsZip(const std::string& path);

}  // namespace zk
