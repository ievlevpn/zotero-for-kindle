#pragma once
// EPUB engine on crengine (inside libzk-epub.so only).

#include <memory>

#include "docs/document.h"

class LVDocView;

namespace zk {

class EpubDom;

class EpubEngine final : public DocumentEngine {
public:
    explicit EpubEngine(const EngineEnv&);
    ~EpubEngine() override;

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
    std::string zoteroPosition(int page) override;
    int pageOfZoteroPosition(const std::string& json) override;
    int64_t wordsBetween(int fromPage, int toPage) override;

private:
    void applyLayout();
    // With two columns our "page" is a spread of crengine's pages: first page of a spread, and back.
    int cols() const { return layout_.columns > 1 ? 2 : 1; }
    int crPage(int spread) const { return spread * cols(); }
    int spreadOf(int crPage) const { return crPage < 0 ? crPage : crPage / cols(); }
    void settle();

    EngineEnv env_;
    Layout layout_;
    std::unique_ptr<LVDocView> view_;
    std::unique_ptr<EpubDom> dom_;
    std::vector<TocEntry> toc_;
};

}  // namespace zk
