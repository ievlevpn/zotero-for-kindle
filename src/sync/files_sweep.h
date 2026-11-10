#pragma once
// Leftovers in <files>, the downloaded attachments' folder (<files>/<attachmentKey>/<name>): the
// folders of attachments no longer in the library, and the .<key>.part and .<key>.zip.part a
// download leaves when the app is killed mid-way (nothing resumes them).

#include <string>
#include <unordered_set>
#include <vector>

namespace zk {

struct FilesSweep {
    std::vector<std::string> removed;  // names in <files>, as removed
};

// Best effort, never throws. Looks only at entries directly in filesDir named like a Zotero key
// (8 of A-Z 0-9): a folder whose key isn't in `attachmentKeys`, and `.<key>.part` or
// `.<key>.zip.part`. Never follows a symbolic link: one is left alone, whatever its name.
// Only safe while nothing downloads into filesDir.
FilesSweep sweepFiles(const std::string& filesDir, const std::unordered_set<std::string>& attachmentKeys);

}  // namespace zk
