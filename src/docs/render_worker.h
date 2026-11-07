#pragma once
// The render worker (spec §7.2): one thread owns the open document engine. The reader asks
// for a page; the worker posts it (from its cache if ready), then pre-renders the next and
// the previous page so a turn is a blit. Requests overtaken by a newer one are not posted.

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "docs/document.h"

namespace zk {

struct RenderEvent {
    enum class Kind { Opened, Page, SearchResults, Error };
    Kind kind = Kind::Page;
    int page = 0, pageCount = 0;
    std::shared_ptr<const PageImage> image;
    std::vector<HighlightBox> highlights;
    std::string chapter;
    double progress = 0;             // 0..1
    int64_t wordsLeftInChapter = 0;  // from this page to the next chapter
    std::vector<TocEntry> toc;       // Opened
    std::vector<SearchHit> hits;     // SearchResults
    UserError error;                 // Error
    DocPosition position;            // of this page
};

class RenderWorker {
public:
    using Factory = std::function<std::unique_ptr<DocumentEngine>(DocType)>;

    explicit RenderWorker(Factory);
    ~RenderWorker();
    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    void open(std::string path, DocType, Layout, DocPosition start);
    void setLayout(Layout, DocPosition keep);  // re-paginates; Opened again, at `keep`
    void show(int page);
    void showPosition(DocPosition);
    void showAnnotation(AnnotationRecord);  // the page the annotation is on (ignored if it can't be placed)
    void search(std::string query);
    void highlightsFor(std::vector<AnnotationRecord>);
    bool poll(RenderEvent&);  // UI thread, non-blocking

private:
    struct Target {
        enum class Kind { None, Page, Position, Annotation } kind = Kind::None;
        int page = 0;
        DocPosition position;
        AnnotationRecord annotation;
    };
    struct OpenRequest {
        std::string path;
        DocType type;
        Layout layout;
        DocPosition start;
    };
    struct Entry {
        std::shared_ptr<const PageImage> image;
        std::vector<HighlightBox> highlights;
    };

    void run();
    void emit(RenderEvent);
    bool hasNewWork();  // caller holds no lock
    Entry& entry(int page);
    RenderEvent pageEvent(int page);
    void relaid(int page);

    Factory make_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    // Requests (guarded by mutex_).
    std::optional<OpenRequest> open_;
    std::optional<std::pair<Layout, DocPosition>> layout_;
    Target target_;
    uint64_t generation_ = 0;
    std::optional<std::string> search_;
    std::optional<std::vector<AnnotationRecord>> annotations_;
    std::deque<RenderEvent> events_;
    // Worker-only state.
    std::unique_ptr<DocumentEngine> engine_;
    std::vector<AnnotationRecord> anns_;
    std::vector<TocEntry> toc_;
    std::map<int, Entry> cache_;
    int current_ = -1;
    int pageCount_ = 0;
};

}  // namespace zk
