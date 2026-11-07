#pragma once
// Inside each engine module: every call is wrapped so no C++ exception crosses the module
// boundary (each module has its own static libstdc++ on the Kindle; an exception thrown here
// can't be relied on to be caught by the app). Failures become safe defaults / a UserError.

#include <memory>

#include "docs/document.h"

namespace zk {

class GuardedEngine final : public DocumentEngine {
public:
    explicit GuardedEngine(std::unique_ptr<DocumentEngine> e) : e_(std::move(e)) {}
    ~GuardedEngine() override {
        try {
            e_.reset();
        } catch (...) {
        }
    }

    bool open(const std::string& path, UserError* err) override {
        try {
            return e_->open(path, err);
        } catch (...) {
            if (err) *err = UserError{ErrorKind::Other, "This book is damaged and can't be opened.", "exception while opening"};
            return false;
        }
    }
    void setLayout(const Layout& l) override { guard([&] { e_->setLayout(l); }); }
    int pageCount() override { return guard([&] { return e_->pageCount(); }, 0); }
    PageImage render(int page) override { return guard([&] { return e_->render(page); }, PageImage{}); }
    DocPosition positionOfPage(int page) override { return guard([&] { return e_->positionOfPage(page); }, DocPosition{}); }
    int pageOf(const DocPosition& p) override { return guard([&] { return e_->pageOf(p); }, 0); }
    std::vector<TocEntry> toc() override { return guard([&] { return e_->toc(); }, std::vector<TocEntry>{}); }
    std::string chapterAt(int page) override { return guard([&] { return e_->chapterAt(page); }, std::string{}); }
    std::vector<SearchHit> search(const std::string& q, int n) override {
        return guard([&] { return e_->search(q, n); }, std::vector<SearchHit>{});
    }
    std::vector<HighlightBox> highlights(int page, const std::vector<AnnotationRecord>& a) override {
        return guard([&] { return e_->highlights(page, a); }, std::vector<HighlightBox>{});
    }
    int pageOfAnnotation(const AnnotationRecord& a) override { return guard([&] { return e_->pageOfAnnotation(a); }, -1); }
    int64_t wordsBetween(int from, int to) override { return guard([&] { return e_->wordsBetween(from, to); }, int64_t{0}); }
    std::optional<Selection> select(int page, Point from, Point to) override {
        return guard([&] { return e_->select(page, from, to); }, std::optional<Selection>{});
    }
    std::optional<std::string> annotationAt(int page, Point p, const std::vector<AnnotationRecord>& a) override {
        return guard([&] { return e_->annotationAt(page, p, a); }, std::optional<std::string>{});
    }

private:
    template <class F, class T> T guard(F&& f, T fallback) {
        try {
            return f();
        } catch (...) {
            return fallback;
        }
    }
    template <class F> void guard(F&& f) {
        try {
            f();
        } catch (...) {
        }
    }
    std::unique_ptr<DocumentEngine> e_;
};

}  // namespace zk
