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
    enum class Kind { Opened, Page, SearchResults, Error, Selection, HitAnnotation, Placed };
    Kind kind = Kind::Page;
    int page = 0, pageCount = 0;
    std::shared_ptr<const PageImage> image;
    std::vector<HighlightBox> highlights;
    std::string chapter;
    double progress = 0;             // 0..1
    int64_t wordsLeftInChapter = 0;  // from this page to the next chapter (estimated far ahead)
    std::vector<TocEntry> toc;       // Opened
    std::vector<SearchHit> hits;     // SearchResults
    uint64_t requestId = 0;          // SearchResults, Selection, HitAnnotation: the id the request returned
    std::optional<Selection> selection;  // Selection (nullopt: not on text)
    std::string annotationKey;           // HitAnnotation ("" if none)
    UserError error;                 // Error
    DocPosition position;            // of this page
    std::string zoteroPosition;      // Page: this page in Zotero's terms (lastPageIndex), "" if none
                                     // Placed: page (-1 if it can't be placed), chapter, progress
};

class RenderWorker {
public:
    using Factory = std::function<std::unique_ptr<DocumentEngine>(DocType)>;

    explicit RenderWorker(Factory);
    ~RenderWorker();
    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // `zoteroStart`: where to open when `start` is empty (Zotero's position, from another device).
    void open(std::string path, DocType, Layout, DocPosition start, std::string zoteroStart = "");
    void setLayout(Layout, DocPosition keep);  // re-paginates; Opened again, at `keep`
    void show(int page);
    void showPosition(DocPosition);
    void showAnnotation(AnnotationRecord);  // the page the annotation is on (ignored if it can't be placed)
    uint64_t search(std::string query);     // results come back with this request id
    void highlightsFor(std::vector<AnnotationRecord>);
    // Text between two points on `page`; only the latest request is answered.
    uint64_t select(int page, Point from, Point to);
    // The annotation drawn under `p` on `page` (of those given to highlightsFor).
    uint64_t annotationAt(int page, Point p);
    // Where a Zotero position (lastPageIndex) falls: a Placed event with this request id.
    uint64_t placeZotero(std::string json);
    bool poll(RenderEvent&);  // UI thread, non-blocking

    // Waits (up to the timeout) for workers whose readers are gone to finish; before exit.
    static void waitForAll(int timeoutMs);
    int64_t wordPagesCounted() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace zk
