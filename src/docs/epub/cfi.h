#pragma once
// EPUB CFIs as Zotero's reader (epub.js lineage) writes them, mapped onto crengine's DOM.
// Rules: research/spikes/s1-cfi.md ("Rules the converter must follow"). CFIs are computed on
// the original XHTML of each spine item (whitespace kept), which is paired node by node with
// crengine's tree (boxing wrappers flattened, whitespace-only text dropped).
// Lives inside libzk-epub.so (and the desktop tests); never throws.

#include <crengine.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pugixml.hpp>

namespace zk {

struct CfiRange {
    int spineIndex = 0;
    ldomXPointer start, end;
};

// A Kindle-made position in Zotero's terms: the CFI, the sort index (rule 6) and the
// annotation text as Zotero stores it (rule 8).
struct GeneratedPosition {
    std::string cfi, sortIndex, text;
};

class EpubDom {
public:
    EpubDom(ldomDocument* doc, const std::string& epubPath);
    ~EpubDom();
    EpubDom(const EpubDom&) = delete;
    EpubDom& operator=(const EpubDom&) = delete;

    std::optional<CfiRange> resolve(const std::string& cfi);
    std::string rangeText(const CfiRange&);
    // The Zotero position of a crengine range (both ends in text nodes of one spine item).
    std::optional<GeneratedPosition> generate(const ldomXPointer& start, const ldomXPointer& end);
    // §7.5: our CFI resolves back to exactly the generated text.
    bool roundTrips(const GeneratedPosition&);

private:
    struct Section;
    Section* section(int index);
    int sectionOf(ldomNode* node);  // spine index of the DocFragment holding `node`, or -1
    std::string readEntry(const std::string& path);

    ldomDocument* doc_;
    LVContainerRef arc_;
    std::vector<std::string> spine_;  // full paths inside the zip
    std::map<int, std::unique_ptr<Section>> sections_;
};

}  // namespace zk
