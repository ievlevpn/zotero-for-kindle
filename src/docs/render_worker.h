#pragma once
// The render worker (spec §7.2): one thread owns the open document engine. The reader asks
// for a page; the worker posts it (from its cache if ready), then pre-renders the next and
// the previous page so a turn is a blit. Requests overtaken by a newer one are not posted.
// Destroying a worker never waits: the thread finishes its current engine call on its own,
// tears the engine down there, and exits (main calls waitForAll() before the process ends).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "docs/document.h"

namespace zk {

struct RenderEvent {
    // Words: the page's time left counted more exactly, after the page (page, wordsLeftInChapter).
    enum class Kind { Opened, Page, SearchResults, Error, Selection, HitAnnotation, Placed, Ink, Link, Area, Words };
    Kind kind = Kind::Page;
    int page = 0, pageCount = 0;
    int viewsPerPage = 1;  // PDF column view: pages are views, this many per PDF page
    std::shared_ptr<const PageImage> image;
    std::vector<HighlightBox> highlights;
    std::string chapter;
    double progress = 0;             // 0..1
    // From this page to the next chapter: the pages already counted, scaled to the chapter (a Words
    // event follows when counting the rest of the next pages, while idle, changes it).
    int64_t wordsLeftInChapter = 0;
    std::vector<TocEntry> toc;       // Opened
    std::vector<SearchHit> hits;     // SearchResults
    uint64_t requestId = 0;          // SearchResults, Selection, HitAnnotation: the id the request returned
    std::optional<Selection> selection;  // Selection (nullopt: not on text)
    std::string annotationKey;           // HitAnnotation ("" if none)
    std::optional<InkPlacement> ink;     // Ink, Area (nullopt: the format has none)
    UserError error;                 // Error
    DocPosition position;            // of this page
    // Page: each bookmark given to setBookmarks with its page (-1: can't be placed), by position.
    std::vector<std::pair<std::string, int>> bookmarkPlaces;
    bool landed = false;  // Page: where a show/showPosition/showAnnotation request took it
    uint64_t layoutId = 0;  // Page: the setLayout it is laid out in (0: the one open() gave)
    std::vector<LinkBox> links;      // Page: the page's links
    Rect pageBox;                    // Page: where the page itself is drawn (empty: all of it)
    std::optional<LinkInfo> link;    // Link: what the asked-for link is (nullopt: nothing to follow)
    std::string zoteroPosition;      // Page: this page in Zotero's terms (lastPageIndex), "" if none
                                     // Placed: page (-1 if it can't be placed), chapter, progress
};

// Rendered pages kept around the current one (the current, then the next, the previous and the one
// after the next) are at most this many bytes. Color pages: three gray pages of the largest screen
// (the Scribe's 1860 × 2480), so only two color ones on a Colorsoft (spec §Design 5: fewer pages,
// the same memory). Gray pages: four of the largest screen there is (the Scribe ColorSoft's
// 1986 × 2648), so two quick turns ahead are both already drawn there too.
constexpr size_t kPageCacheBytes = size_t{3} * 1860 * 2480;
constexpr size_t kGrayPageCacheBytes = size_t{4} * 1986 * 2648;
// The budget for pages of `pageBytes` each: kPageCacheBytes (kGrayPageCacheBytes for gray ones), but
// always room for the current page and the next (a Scribe ColorSoft's color page is about the whole
// of kPageCacheBytes).
size_t pageCacheBudget(size_t pageBytes, bool gray = false);
// The pages kept rendered around `current`, in the order they are drawn: it, the next, the previous,
// the one after the next, while they fit pageCacheBudget at `pageBytes` each (all of a document's
// pages are the same size).
std::vector<int> pagesToKeep(int current, int pageCount, size_t pageBytes, bool gray = false);

// The saturation boost color pages get (spec §Design 4; page content only, never the UI): each
// pixel moved away from its HSP brightness by `pct` percent; grays stay exactly as they are.
// `rgb`: `px` pixels of R, G, B.
void boostSaturation(uint8_t* rgb, size_t px, int pct);
// ZK_COLOR_BOOST (`env`, nullptr when unset): 0..100, else the default 25.
int colorBoostPct(const char* env);

class RenderWorker {
public:
    using Factory = std::function<std::unique_ptr<DocumentEngine>(DocType)>;

    explicit RenderWorker(Factory);
    ~RenderWorker();
    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // `zoteroStart`: where to open when `start` is empty (Zotero's position, from another device).
    // `color`: pages in color (DocumentEngine::setColor), boosted by `boostPct` (colorBoostPct).
    void open(std::string path, DocType, Layout, DocPosition start, std::string zoteroStart = "", bool color = false,
              int boostPct = 0);
    // Display → Color changed: the pages are rendered again, in color or gray; the current one is posted.
    void setColor(bool on, int boostPct);
    // Re-paginates; Opened again, at `keep`. Pages laid out with it carry the id (RenderEvent::layoutId).
    uint64_t setLayout(Layout, DocPosition keep);
    void show(int page);
    void showPosition(DocPosition);
    void showAnnotation(AnnotationRecord);  // the page the annotation is on (ignored if it can't be placed)
    void showZotero(std::string position);  // a Zotero position, placed in the pages as they are then (ignored if it can't be)
    uint64_t search(std::string query);     // results come back with this request id
    void highlightsFor(std::vector<AnnotationRecord>);
    // Text between two points on `page`; only the latest request is answered.
    uint64_t select(int page, Point from, Point to);
    // The annotation drawn under `p` on `page` (of those given to highlightsFor).
    uint64_t annotationAt(int page, Point p);
    // Where a Zotero position (lastPageIndex) falls: a Placed event with this request id.
    uint64_t placeZotero(std::string json);
    // The book's bookmarks: each Page event says which page each one is on (the page is posted again).
    void setBookmarks(std::vector<DocPosition>);
    // What link `index` on `page` is: a Link event with this request id.
    uint64_t linkAt(int page, int index);
    // Handwriting on `page` placed for Zotero; every request is answered, in order.
    uint64_t ink(int page, std::vector<std::vector<Point>> paths, int widthPx);
    // A rectangle on `page` placed for Zotero (an Area event); answered in order with ink.
    uint64_t area(int page, Rect r);
    // A sticky note tapped at `at` on `page` placed for Zotero (also an Area event, in order).
    uint64_t note(int page, Point at);
    // Empties the engine's resource cache (the app went to the background); pages drawn stay.
    void trimCaches();
    bool poll(RenderEvent&);  // UI thread, non-blocking

    // Waits (up to the timeout) for workers whose readers are gone to finish; before exit.
    static void waitForAll(int timeoutMs);
    int64_t wordPagesCounted() const;
    // The most bytes of rendered pages the worker has held at once (for tests of the budget).
    size_t peakCachedBytes() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace zk
