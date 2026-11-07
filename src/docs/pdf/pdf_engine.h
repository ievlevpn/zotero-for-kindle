#pragma once
// PDF engine on MuPDF (inside libzk-pdf.so only). Pages are fitted into the layout area,
// centred, in gray. Zotero PDF positions are {pageIndex, rects:[[x1,y1,x2,y2]…]} in PDF user
// space (origin bottom-left); they're mapped through the page transform to device pixels.

#include "docs/document.h"

struct fz_context;
struct fz_document;

namespace zk {

class PdfEngine final : public DocumentEngine {
public:
    PdfEngine();
    ~PdfEngine() override;

    bool open(const std::string& path, UserError*) override;
    void setLayout(const Layout&) override;
    int pageCount() override;
    PageImage render(int page) override;
    DocPosition positionOfPage(int page) override;
    int pageOf(const DocPosition&) override;
    std::vector<TocEntry> toc() override;
    std::string chapterAt(int page) override;
    std::vector<SearchHit> search(const std::string& query, int maxHits) override;
    std::vector<HighlightBox> highlights(int page, const std::vector<AnnotationRecord>&) override;
    std::optional<Selection> select(int page, Point from, Point to) override;
    int pageOfAnnotation(const AnnotationRecord&) override;
    int64_t wordsBetween(int fromPage, int toPage) override;

private:
    std::string pageText(int page);
    // Page space -> device: scale and offset for fitting page `page` into the layout area.
    bool fit(int page, double& scale, double& dx, double& dy);

    fz_context* ctx_ = nullptr;
    fz_document* doc_ = nullptr;
    Layout layout_;
    std::vector<TocEntry> toc_;
    bool tocLoaded_ = false;
};

}  // namespace zk
