#pragma once
// Annotation text as Zotero's reader stores it (spike S1 rule 8): within a block every run of
// whitespace (NBSP and BOM included) becomes one space; blocks are separated by a blank line;
// the ends are trimmed. Input blocks are separated by '\n' (crengine's range text).

#include <string>

namespace zk {

std::string normaliseAnnotationText(const std::string& text);

}  // namespace zk
