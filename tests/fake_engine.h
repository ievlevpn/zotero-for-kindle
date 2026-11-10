#pragma once
// A deterministic DocumentEngine for reader tests: N pages; page i is filled with gray level
// (i % 200) so tests can tell pages apart; chapters every `chapterEvery` pages; optional delay.
// Positions are "pos:<k>" where k = page × layout.sizePt × 10 — changing the size moves a
// position to another page, like re-pagination.
// Text: lines of 10 words, each word a 100×50 box from x = 100, lines every 60 px from y = 200
// (8 lines); word n of a page is "p<page>w<n>".

#include "docs/document.h"

#include <nlohmann/json.hpp>

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
    Rect box;  // pageBox(): where the page is drawn (empty: all of it)
    std::atomic<int64_t> wordPagesCounted{0};  // pages whose words were counted
    std::atomic<int> renders{0};

    bool open(const std::string&, UserError* err) override {
        if (openDelayMs) std::this_thread::sleep_for(std::chrono::milliseconds(openDelayMs));
        if (failOpen && err) *err = UserError{ErrorKind::Other, "This EPUB is damaged and can't be opened.", "fake"};
        return !failOpen;
    }
    void setLayout(const Layout& l) override { layout = l; }
    // PDF column view: three views per page; positions and Zotero positions stay pages.
    int viewsPerPage() override { return layout.columnView ? 3 : 1; }
    int pageCount() override { return static_cast<int>(pages * 9 / layout.sizePt) * viewsPerPage(); }
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
        page /= viewsPerPage();
        return {"pos:" + std::to_string(static_cast<long>(std::lround(page * layout.sizePt * 10)))};
    }
    int pageOf(const DocPosition& p) override {
        if (p.value.rfind("pos:", 0) != 0) return 0;
        return static_cast<int>(std::lround(std::stol(p.value.substr(4)) / (layout.sizePt * 10))) * viewsPerPage();
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
            if (a.pageLabel == std::to_string(page) && a.type == "ink") {  // position: device points as drawn
                HighlightBox b{a.key, {}, 0, false};
                b.kind = HighlightBox::Kind::Ink;
                b.strokePx = 4;
                const nlohmann::json pos = nlohmann::json::parse(a.position);
                for (const auto& xy : pos["paths"]) {
                    std::vector<Point> path;
                    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
                        path.push_back({xy[i].get<int>(), xy[i + 1].get<int>()});
                        b.rect = b.rect.united({path.back().x - 4, path.back().y - 4, 9, 9});
                    }
                    b.paths.push_back(std::move(path));
                }
                out.push_back(std::move(b));
            } else if (a.pageLabel == std::to_string(page) && a.type == "image" &&
                       a.position.find("\"rect\"") != std::string::npos) {  // position: the device rect as drawn
                const nlohmann::json r = nlohmann::json::parse(a.position)["rect"];
                HighlightBox b{a.key, {r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), r[3].get<int>()}, shadeOf(a.color), false};
                b.kind = HighlightBox::Kind::Image;
                out.push_back(b);
            } else if (a.pageLabel == std::to_string(page) && a.type == "note" &&
                       a.position.find("\"note\"") != std::string::npos) {  // position: the tap as drawn
                const nlohmann::json n = nlohmann::json::parse(a.position)["note"];
                HighlightBox b{a.key, {n[0].get<int>(), n[1].get<int>(), 40, 40}, shadeOf(a.color), false};
                b.kind = HighlightBox::Kind::Note;
                out.push_back(b);
            } else if (a.pageLabel == std::to_string(page)) {
                HighlightBox b{a.key, {100, 200, 600, 50}, shadeOf(a.color), a.type == "underline"};
                if (a.type == "text") {
                    b.kind = HighlightBox::Kind::Text;
                    b.text = a.comment;
                    b.fontPx = 36;
                }
                out.push_back(b);
            }
        return out;
    }
    int pageOfPlaced(const DocPosition& p) override {
        if (p.value.rfind("pos:", 0) != 0) return -1;
        const int page = pageOf(p);
        return page >= 0 && page < pageCount() ? page : -1;
    }
    // Links on every page ending in 3: a note, a jump (20 pages on), a web address, a note on a
    // dagger 4 px wide, a web address running into the right edge, and a PDF-style reference
    // (30 pages on, shown in a pop-up with Go to page).
    std::vector<LinkBox> links(int page) override {
        if (page % 10 != 3) return {};
        return {{{200, 400, 60, 40}, 0}, {{500, 400, 120, 40}, 1}, {{800, 400, 120, 40}, 2},
                {{300, 900, 4, 10}, 3},  {{900, 1200, 330, 40}, 4},  {{600, 1000, 100, 40}, 5}};
    }
    std::optional<LinkInfo> link(int page, int index) override {
        if (page % 10 != 3 || index < 0 || index > 5) return std::nullopt;
        LinkInfo l;
        if (index == 5) {
            l.kind = LinkInfo::Kind::Reference;
            l.targetPage = page + 30;
            l.label = "Page " + std::to_string(l.targetPage / viewsPerPage() + 1);
            l.text = "[12] A reference.";
            return l;
        }
        if (index == 0 || index == 3) {
            l.kind = LinkInfo::Kind::Note;
            l.label = "Footnote 3";
            l.text = "See Zosimus, l. ii. p. 104, who allows the prudence of the measure.";
            l.targetPage = page + 50;
        } else if (index == 1) {
            l.kind = LinkInfo::Kind::Jump;
            l.targetPage = page + 20;
        } else {
            l.kind = LinkInfo::Kind::Web;
            l.label = "Web link";
            l.url = "https://example.org/pinns";
            return l;
        }
        l.target = positionOfPage(l.targetPage);
        return l;
    }
    // Zotero positions: the page number as text.
    std::string zoteroPosition(int page) override { return std::to_string(page / viewsPerPage()); }
    int pageOfZoteroPosition(const std::string& v) override {
        try {
            const int p = std::stoi(v) * viewsPerPage();
            return p >= 0 && p < pageCount() ? p : -1;
        } catch (...) {
            return -1;
        }
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

    std::optional<InkPlacement> inkPosition(int page, const std::vector<std::vector<Point>>& paths, int widthPx) override {
        nlohmann::json jp = nlohmann::json::array();
        for (const auto& path : paths) {
            nlohmann::json xy = nlohmann::json::array();
            for (const Point& q : path) xy.push_back(q.x), xy.push_back(q.y);
            jp.push_back(xy);
        }
        char sort[32];
        std::snprintf(sort, sizeof sort, "%05d|%08d", page, 0);
        return InkPlacement{nlohmann::json{{"fake", page}, {"width", widthPx}, {"paths", jp}}.dump(), sort,
                            std::to_string(page)};
    }

    Rect pageBox(int) override { return box; }

    // Areas: the device rect as drawn, {"fake": page, "rect": [x, y, w, h]}.
    std::optional<InkPlacement> areaPosition(int page, Rect r) override {
        char sort[32];
        std::snprintf(sort, sizeof sort, "%05d|%08d", page, r.y);
        return InkPlacement{nlohmann::json{{"fake", page}, {"rect", {r.x, r.y, r.w, r.h}}}.dump(), sort, std::to_string(page)};
    }

    // Notes: the tap as drawn, {"fake": page, "note": [x, y]}.
    std::optional<InkPlacement> notePosition(int page, Point at) override {
        char sort[32];
        std::snprintf(sort, sizeof sort, "%05d|%08d", page, at.y);
        return InkPlacement{nlohmann::json{{"fake", page}, {"note", {at.x, at.y}}}.dump(), sort, std::to_string(page)};
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
