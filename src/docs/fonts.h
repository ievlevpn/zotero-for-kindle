#pragma once
// Reading fonts: the families found in the font directories (spec §3: v1 offers the fonts on
// the device; more fonts later = dropping files into /mnt/us/zotero/fonts).

#include <string>
#include <vector>

namespace zk {

// Families with an upright regular face, de-duplicated, Bookerly first, then alphabetical.
std::vector<std::string> readingFonts(const std::vector<std::string>& dirs);
std::vector<std::string> readingFontDirs();

}  // namespace zk
