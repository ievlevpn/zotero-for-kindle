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

// A link on a page (device pixels), by its index on that page.
struct LinkBox {
    Rect rect;
    int index = 0;
};

// What a link is. Note: a footnote or endnote (shown in a pop-up, "Go to note"). Reference: a
// PDF's internal link, e.g. a citation (the text at its target in a pop-up, "Go to page N").
// Jump: an EPUB link to another part of the book. Web: an address outside the book.
struct LinkInfo {
    enum class Kind { Note, Reference, Jump, Web } kind = Kind::Jump;
    std::string label;  // "Footnote 3", "Page 14", "Web link"
    std::string text;   // the note or the text at the target ("" for a jump)
    std::string url;    // Web
    std::string mark;   // the link's own text ("26", "II", "[12]")
    DocPosition target;
    int targetPage = -1;
};

struct SearchHit {
    int page = 0;
    std::string snippet;
    DocPosition pos;
};

struct Layout {
    std::string font = "Bookerly";
    double sizePt = 9;
    int marginPx = 60;      // from marginLevel and the page width (marginPxFor), set by the reader
    int lineSpacingPct = 120;
    int width = 1236, height = 1547 - 60;  // the page area: set by the reader from the window
    int marginLevel = 1;    // 0 Narrow, 1 Normal, 2 Wide, 3 Extra wide (EPUB)
    bool trimMargins = false;  // PDF: crop the page's white border so the text is larger
    bool columnView = false;   // PDF: one column at a time, fitted to the width, in slices
    int columns = 1;        // EPUB: 2 in landscape (pages are then spreads of two)
    bool operator==(const Layout&) const = default;
};

// Margins as a share of the page width, so they suit every screen: 40, 60, 100 px on the PW5
// (as before), wider on the Scribe; and back from an old per-book pixel setting.
inline int marginPxFor(int level, int pageWidth) {
    static const double share[] = {0.0324, 0.0486, 0.081, 0.13};
    const int i = level < 0 ? 0 : level > 3 ? 3 : level;
    return static_cast<int>(pageWidth * share[i] + 0.5);
}
inline int marginLevelFor(int marginPx, int pageWidth) {
    int best = 1, bestDiff = 1 << 30;
    for (int i = 0; i < 4; ++i) {
        const int d = marginPxFor(i, pageWidth) - marginPx;
        if ((d < 0 ? -d : d) < bestDiff) bestDiff = d < 0 ? -d : d, best = i;
    }
    return best;
}

struct HighlightBox {
    std::string annotationKey;
    Rect rect;
    uint8_t shade = 200;
    bool underline = false;
    // Zotero's other annotation types (PDF): a text box shows its words, a sticky note an icon,
    // an image annotation its area, ink its strokes.
    enum class Kind { Highlight, Text, Note, Image, Ink };
    Kind kind = Kind::Highlight;
    std::string text;                       // Text: what it says
    int fontPx = 0;                         // Text: its font size on the page
    std::vector<std::vector<Point>> paths;  // Ink: device points
    int strokePx = 3;                       // Ink: pen width on the page
};

// Text the reader selected, with everything Zotero needs to place an annotation on it.
struct Selection {
    std::string text;         // as Zotero stores annotationText
    std::vector<Rect> rects;  // device pixels on the page, one per line
    std::string position;     // Zotero annotationPosition JSON
    std::string sortIndex, pageLabel;
    bool roundTrips = true;   // §7.5: the position resolves back to exactly this text
};

// Handwriting placed on a page, ready to be saved as a Zotero `ink` annotation.
struct InkPlacement {
    std::string position;  // Zotero annotationPosition JSON {pageIndex, width, paths}
    std::string sortIndex, pageLabel;
};

// The most text fullText() may return; an engine stops once it has gone over (the caller then
// refuses the file).
constexpr size_t kMaxFullTextBytes = 32u * 1024 * 1024;

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
    // PDF column view: the engine's pages are views, this many per PDF page (1: pages are pages).
    virtual int viewsPerPage() { return 1; }
    virtual PageImage render(int page) = 0;
    virtual DocPosition positionOfPage(int page) = 0;
    virtual int pageOf(const DocPosition&) = 0;
    // As pageOf, but -1 where the position can't be placed (a bookmark from another edition).
    virtual int pageOfPlaced(const DocPosition& p) { return pageOf(p); }
    // A page's links, and what one of them is (nullopt: not a link we can follow).
    virtual std::vector<LinkBox> links(int /*page*/) { return {}; }
    virtual std::optional<LinkInfo> link(int /*page*/, int /*index*/) { return std::nullopt; }
    virtual std::vector<TocEntry> toc() = 0;
    virtual std::string chapterAt(int page) = 0;
    virtual std::vector<SearchHit> search(const std::string& query, int maxHits) = 0;
    virtual std::vector<HighlightBox> highlights(int page, const std::vector<AnnotationRecord>&) = 0;
    virtual int pageOfAnnotation(const AnnotationRecord&) = 0;  // -1 if it can't be placed
    // The reading position as Zotero syncs it (setting lastPageIndex_u_<attachmentKey>), as JSON:
    // a PDF's page index (2), an EPUB's point CFI at the page's start ("/6/24!/4/2/2/3:10").
    // "" if there is none. pageOfZoteroPosition is the way back; -1 if it can't be placed.
    virtual std::string zoteroPosition(int /*page*/) { return ""; }
    virtual int pageOfZoteroPosition(const std::string& /*json*/) { return -1; }
    virtual int64_t wordsBetween(int fromPage, int toPage) = 0;  // for "time left"
    // As open(), for a caller that only wants fullText(): no layout or pagination (EPUB), so the
    // shared crengine isn't held for seconds. The engine can then only fullText().
    virtual bool openForText(const std::string& path, UserError* err) { return open(path, err); }
    // The most the engine's own cache of decoded resources may hold (0: none it limits itself).
    // PDF: MuPDF's store, smaller after openForText (see pdf_engine.h).
    virtual size_t cacheLimitBytes() { return 0; }
    // Empty that cache (the app went to the background); it refills as pages are drawn.
    virtual void trimCaches() {}
    // The whole document's text into `out`, UTF-8, in reading order, for plugins. EPUB: paragraphs
    // separated by "\n\n". PDF: each page's text, pages separated by "\f" (a page without text
    // layer is empty: a scan gives only form feeds, still a success). Stops (leaving more than
    // kMaxFullTextBytes in `out`) once over the limit. False with *err on a failure; never "".
    virtual bool fullText(std::string& /*out*/, UserError* err) {
        if (err) *err = UserError{ErrorKind::Other, "This file's text can't be read.", "fullText not supported"};
        return false;
    }
    // The whole words between two points on a page (in either order); nullopt if `from` is not on
    // text. A Selection with empty text means the page has no text layer at all (a scan).
    virtual std::optional<Selection> select(int page, Point from, Point to) = 0;
    // Strokes drawn on `page` (device points, pen `widthPx` wide) as a Zotero ink annotation;
    // nullopt where the format has no ink (EPUB).
    virtual std::optional<InkPlacement> inkPosition(int /*page*/, const std::vector<std::vector<Point>>& /*paths*/,
                                                    int /*widthPx*/) {
        return std::nullopt;
    }
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
