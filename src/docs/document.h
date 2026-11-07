#pragma once
// One interface for both readers (spec §7.3 `docs/`). Engines live in their own shared modules
// (libzk-epub.so, libzk-pdf.so) so their freetype/harfbuzz never meet the firmware's (pango).
// Nothing here throws across the module boundary; errors come back as UserError.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/geometry.h"
#include "net/errors.h"
#include "store/records.h"

namespace zk {

enum class DocType { Epub, Pdf };

struct PageImage {
    int width = 0, height = 0;
    std::vector<uint8_t> gray;  // width*height, 0 = black
};

struct DocPosition {
    std::string value;  // engine-native: crengine xpointer, or "p:<pageIndex>" for PDF
    bool operator==(const DocPosition&) const = default;
};

struct TocEntry {
    std::string title;
    int level = 0;
    DocPosition pos;
    int page = 0;
};

struct SearchHit {
    int page = 0;
    std::string snippet;
    DocPosition pos;
};

struct Layout {
    std::string font = "Bookerly";
    double sizePt = 9;
    int marginPx = 60;
    int lineSpacingPct = 120;
    int width = 1236, height = 1547 - 60;  // the page area above the reader's footer
    bool operator==(const Layout&) const = default;
};

struct HighlightBox {
    std::string annotationKey;
    Rect rect;
    uint8_t shade = 200;
    bool underline = false;
};

// Text the reader selected, with everything Zotero needs to place an annotation on it.
struct Selection {
    std::string text;         // as Zotero stores annotationText
    std::vector<Rect> rects;  // device pixels on the page, one per line
    std::string position;     // Zotero annotationPosition JSON
    std::string sortIndex, pageLabel;
    bool roundTrips = true;   // §7.5: the position resolves back to exactly this text
};

// Where an engine finds its data (crengine's hyphenation dictionaries and CSS, fonts, cache).
struct EngineEnv {
    std::string dataDir;
    std::vector<std::string> fontDirs;
    std::string cacheDir;
};

class DocumentEngine {
public:
    virtual ~DocumentEngine() = default;
    virtual bool open(const std::string& path, UserError*) = 0;
    virtual void setLayout(const Layout&) = 0;  // EPUB re-paginates; PDF uses width/height
    virtual int pageCount() = 0;
    virtual PageImage render(int page) = 0;
    virtual DocPosition positionOfPage(int page) = 0;
    virtual int pageOf(const DocPosition&) = 0;
    virtual std::vector<TocEntry> toc() = 0;
    virtual std::string chapterAt(int page) = 0;
    virtual std::vector<SearchHit> search(const std::string& query, int maxHits) = 0;
    virtual std::vector<HighlightBox> highlights(int page, const std::vector<AnnotationRecord>&) = 0;
    virtual int pageOfAnnotation(const AnnotationRecord&) = 0;  // -1 if it can't be placed
    virtual int64_t wordsBetween(int fromPage, int toPage) = 0;  // for "time left"
    // The whole words between two points on a page (in either order); nullopt if `from` is not on
    // text. A Selection with empty text means the page has no text layer at all (a scan).
    virtual std::optional<Selection> select(int page, Point from, Point to) = 0;
    // The key of the topmost annotation drawn under `p` on `page`.
    virtual std::optional<std::string> annotationAt(int page, Point p, const std::vector<AnnotationRecord>& anns) {
        const std::vector<HighlightBox> boxes = highlights(page, anns);
        for (auto it = boxes.rbegin(); it != boxes.rend(); ++it)
            if (it->rect.contains(p)) return it->annotationKey;
        return std::nullopt;
    }
};

// Zotero's annotation colours as gray shades for the e-ink page.
uint8_t shadeOf(const std::string& zoteroColor);

}  // namespace zk
