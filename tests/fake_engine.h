#pragma once
// A deterministic DocumentEngine for reader tests: N pages; page i is filled with gray level
// (i % 200) so tests can tell pages apart; chapters every `chapterEvery` pages; optional delay.
// Positions are "pos:<k>" where k = page × layout.sizePt × 10 — changing the size moves a
// position to another page, like re-pagination.

#include "docs/document.h"

#include <chrono>
#include <cmath>
#include <thread>

namespace zk::test {

class FakeEngine : public DocumentEngine {
public:
    int pages = 100;
    int chapterEvery = 10;
    int delayMs = 0;
    bool failOpen = false;
    std::atomic<int> renders{0};

    bool open(const std::string&, UserError* err) override {
        if (failOpen && err) *err = UserError{ErrorKind::Other, "This EPUB is damaged and can't be opened.", "fake"};
        return !failOpen;
    }
    void setLayout(const Layout& l) override { layout = l; }
    int pageCount() override { return static_cast<int>(pages * 9 / layout.sizePt); }
    PageImage render(int page) override {
        ++renders;
        if (delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        PageImage img;
        img.width = layout.width;
        img.height = layout.height;
        img.gray.assign(static_cast<size_t>(img.width) * img.height, static_cast<uint8_t>(page % 200));
        return img;
    }
    DocPosition positionOfPage(int page) override {
        return {"pos:" + std::to_string(static_cast<long>(std::lround(page * layout.sizePt * 10)))};
    }
    int pageOf(const DocPosition& p) override {
        if (p.value.rfind("pos:", 0) != 0) return 0;
        return static_cast<int>(std::lround(std::stol(p.value.substr(4)) / (layout.sizePt * 10)));
    }
    std::vector<TocEntry> toc() override {
        std::vector<TocEntry> out;
        for (int p = 0; p < pageCount(); p += chapterEvery)
            out.push_back({"Chapter " + std::to_string(p / chapterEvery + 1), 0, positionOfPage(p), p});
        return out;
    }
    std::string chapterAt(int page) override { return "Chapter " + std::to_string(page / chapterEvery + 1); }
    std::vector<SearchHit> search(const std::string& q, int maxHits) override {
        std::vector<SearchHit> out;
        for (int p = 3; p < pageCount() && static_cast<int>(out.size()) < maxHits; p += 7)
            out.push_back({p, "…" + q + " on page " + std::to_string(p) + "…", positionOfPage(p)});
        return out;
    }
    std::vector<HighlightBox> highlights(int page, const std::vector<AnnotationRecord>& anns) override {
        std::vector<HighlightBox> out;
        for (const auto& a : anns)
            if (a.pageLabel == std::to_string(page)) out.push_back({a.key, {100, 200, 600, 50}, shadeOf(a.color), a.type == "underline"});
        return out;
    }
    int64_t wordsBetween(int from, int to) override { return static_cast<int64_t>(to - from) * 300; }

    Layout layout;
};

}  // namespace zk::test
