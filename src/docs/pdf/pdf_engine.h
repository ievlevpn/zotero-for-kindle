#pragma once
// PDF engine on MuPDF (inside libzk-pdf.so only). Pages are fitted into the layout area,
// centred, in gray. Zotero PDF positions are {pageIndex, rects:[[x1,y1,x2,y2]…]} in PDF user
// space (origin bottom-left); they're mapped through the page transform to device pixels.

#include "docs/document.h"

#include <array>
#include <map>

struct fz_context;
struct fz_document;

namespace zk {

class PdfEngine final : public DocumentEngine {
public:
    // MuPDF's resource store (decoded images, fonts) per context. A reader's: a few scanned pages'
    // images (one is ~8 MB in gray); MuPDF evicts beyond it. Text only (plugins' fullText): fonts.
    // Several contexts can be alive at once (reader, fullText, a closing reader's worker) on a
    // 512 MB Kindle, so these stay small.
    static constexpr size_t kReaderStoreBytes = 24u << 20;
    static constexpr size_t kTextStoreBytes = 8u << 20;

    explicit PdfEngine(size_t storeBytes = kReaderStoreBytes);
    ~PdfEngine() override;

    bool open(const std::string& path, UserError*) override;
    bool openForText(const std::string& path, UserError*) override;  // with a kTextStoreBytes store
    size_t cacheLimitBytes() override { return ctx_ ? storeBytes_ : 0; }
    void trimCaches() override;
    void setLayout(const Layout&) override;
    int pageCount() override;
    int viewsPerPage() override;
    PageImage render(int page) override;
    DocPosition positionOfPage(int page) override;
    int pageOf(const DocPosition&) override;
    std::vector<TocEntry> toc() override;
    std::string chapterAt(int page) override;
    std::vector<SearchHit> search(const std::string& query, int maxHits) override;
    std::vector<HighlightBox> highlights(int page, const std::vector<AnnotationRecord>&) override;
    std::optional<Selection> select(int page, Point from, Point to) override;
    std::optional<InkPlacement> inkPosition(int page, const std::vector<std::vector<Point>>& paths, int widthPx) override;
    int pageOfAnnotation(const AnnotationRecord&) override;
    std::string zoteroPosition(int page) override;
    int pageOfZoteroPosition(const std::string& json) override;
    int pageOfPlaced(const DocPosition&) override;
    std::vector<LinkBox> links(int page) override;
    std::optional<LinkInfo> link(int page, int index) override;
    int64_t wordsBetween(int fromPage, int toPage) override;
    bool fullText(std::string& out, UserError*) override;

private:
    // Column view (design 2026-11-08): the engine's pages are views of the PDF's pages.
    // The document's: its text area (sample pages' median width; their highest top and lowest
    // bottom) and columns.
    struct Columns {
        bool analysed = false, text = false;
        int columns = 1;
        float x[3] = {0, 0, 0};  // column edges, page space: x[0]..x[1] (and x[1]..x[2])
        float top = 0, bottom = 0;
    };
    // Per layout: how many slices a column takes (the same on every page).
    struct Slices {
        int count = 1;
        int width = 0, screenHeight = 0;  // the layout they are for
    };
    // Per page: the document's columns moved to where this page's text is (mirrored margins) and
    // grown to hold all of it; each column's slices (a page too tall for them is shown a little
    // smaller, never cut).
    struct PageViews {
        float x[3] = {0, 0, 0};
        float top = 0;
        float width[2] = {0, 0}, height[2] = {0, 0}, step[2] = {0, 0};  // a slice's, per column
    };
    const PageViews& pageViews(int pdfPage);
    bool textBox(int pdfPage, float box[4]);  // the page's text lines' extent
    int pdfPages();
    bool columnsOn();
    int src(int page) { return page / viewsPerPage(); }
    void analyseColumns();
    void sliceColumns();
    // The view of PDF page `pdfPage` that shows point (x, y) of it (page space).
    int viewAt(int pdfPage, float x, float y);
    // The view's area in page space.
    struct Area {
        float x0, y0, x1, y1;
    };
    Area viewRect(int page);
    std::string pageText(int page, bool* failed = nullptr);
    // The text at a link's target (x, y in page space; NaN: the page's top): the entry that
    // starts there, in its column, as far as it goes.
    std::string textAt(int page, float x, float y);
    // Page space -> device: scale and offset for fitting page `page` into the layout area.
    bool fit(int page, double& scale, double& dx, double& dy);
    // The page's inked area (page space), for trimmed margins; measured once per page.
    bool contentBox(int page, float box[4]);

    void newContext(size_t storeBytes);

    fz_context* ctx_ = nullptr;
    size_t storeBytes_ = 0;
    fz_document* doc_ = nullptr;
    Layout layout_;
    std::vector<TocEntry> toc_;  // in PDF pages
    std::vector<std::array<float, 2>> tocPlaces_;  // each entry's x, y on its page (NaN: unknown)
    std::vector<TocEntry> tocInViews();
    bool tocLoaded_ = false;
    std::map<int, std::array<float, 4>> contentBoxes_;
    Columns columns_;
    std::map<int, PageViews> pageViews_;              // per layout
    std::map<int, std::array<float, 4>> textBoxes_;  // per document
    std::map<int, int64_t> pageWords_;  // per PDF page
    Slices slices_;
};

}  // namespace zk
