#pragma once
// A deterministic DocumentEngine for reader tests: N pages; page i is filled with gray level
// (i % 200) so tests can tell pages apart; chapters every `chapterEvery` pages; optional delay.
// Positions are "pos:<k>" where k = page × layout.sizePt × 10 — changing the size moves a
// position to another page, like re-pagination.
// Text: lines of 10 words, each word a 100×50 box from x = 100, lines every 60 px from y = 200
// (8 lines); word n of a page is "p<page>w<n>".

#include "docs/document.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace zk::test {

class FakeEngine : public DocumentEngine {
public:
    int pages = 100;
    int chapterEvery = 10;
    int delayMs = 0;
    bool failOpen = false;
    int openDelayMs = 0;
    bool throwOnRender = false;
    bool throwOnSelect = false;
    bool selectRoundTrips = true;
    std::atomic<int64_t> wordPagesCounted{0};  // pages whose words were counted
    std::atomic<int> renders{0};

    bool open(const std::string&, UserError* err) override {
        if (openDelayMs) std::this_thread::sleep_for(std::chrono::milliseconds(openDelayMs));
        if (failOpen && err) *err = UserError{ErrorKind::Other, "This EPUB is damaged and can't be opened.", "fake"};
        return !failOpen;
    }
    void setLayout(const Layout& l) override { layout = l; }
    int pageCount() override { return static_cast<int>(pages * 9 / layout.sizePt); }
    PageImage render(int page) override {
        ++renders;
        if (throwOnRender) throw std::bad_alloc();
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
        if (q == "zzz") return out;
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
    int pageOfAnnotation(const AnnotationRecord& a) override {
        try {
            return a.pageLabel.empty() ? -1 : std::stoi(a.pageLabel);
        } catch (...) {
            return -1;
        }
    }
    int64_t wordsBetween(int from, int to) override {
        wordPagesCounted += to - from;
        return static_cast<int64_t>(to - from) * 300;
    }

    std::optional<Selection> select(int page, Point from, Point to) override {
        if (throwOnSelect) throw std::runtime_error("select");
        auto word = [](Point p) -> int {
            const int col = (p.x - 100) / 100, line = (p.y - 200) / 60;
            if (p.x < 100 || p.y < 200 || col > 9 || line > 7 || (p.y - 200) % 60 >= 50) return -1;
            return line * 10 + col;
        };
        int a = word(from), b = word(to);
        if (a < 0) return std::nullopt;
        if (b < 0) b = a;
        if (b < a) std::swap(a, b);
        Selection s;
        for (int w = a; w <= b; ++w) {
            s.text += (w == a ? "" : " ") + std::string("p") + std::to_string(page) + "w" + std::to_string(w);
            const Rect box{100 + (w % 10) * 100, 200 + (w / 10) * 60, 100, 50};
            if (!s.rects.empty() && s.rects.back().y == box.y) s.rects.back() = s.rects.back().united(box);
            else s.rects.push_back(box);
        }
        s.position = R"({"fake":)" + std::to_string(page) + R"(,"from":)" + std::to_string(a) + R"(,"to":)" +
                     std::to_string(b) + "}";
        char sort[32];
        std::snprintf(sort, sizeof sort, "%05d|%08d", page, a);
        s.sortIndex = sort;
        s.pageLabel = std::to_string(page);
        s.roundTrips = selectRoundTrips;
        return s;
    }

    Layout layout;
};

}  // namespace zk::test
