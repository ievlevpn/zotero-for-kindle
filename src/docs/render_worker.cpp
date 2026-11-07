#include "docs/render_worker.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "core/log.h"

namespace zk {

namespace {

constexpr int kWordsLookahead = 30;  // pages counted exactly for "time left"; the rest estimated

// Threads still running (their readers may be gone): main waits for them before exiting.
std::mutex g_liveMutex;
std::condition_variable g_liveCv;
int g_live = 0;

}  // namespace

struct RenderWorker::Impl {
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
        std::string zoteroStart;
    };
    struct Entry {
        std::shared_ptr<const PageImage> image;
        std::vector<HighlightBox> highlights;
    };

    Factory make;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    // Requests (guarded by mutex).
    std::optional<OpenRequest> open;
    std::optional<std::pair<Layout, DocPosition>> layout;
    Target target;
    uint64_t generation = 0;
    std::optional<std::pair<uint64_t, std::string>> search;
    uint64_t nextSearchId = 1;
    std::optional<std::vector<AnnotationRecord>> annotations;
    struct PointRequest {
        uint64_t id = 0;
        int page = 0;
        Point from, to;
    };
    std::optional<PointRequest> selectRequest, hitRequest;
    std::optional<std::pair<uint64_t, std::string>> placeRequest;
    uint64_t nextPointId = 1;
    struct InkRequest {
        uint64_t id = 0;
        int page = 0;
        std::vector<std::vector<Point>> paths;
        int widthPx = 0;
    };
    std::deque<InkRequest> inkRequests;
    std::deque<RenderEvent> events;
    // Worker-only state.
    std::unique_ptr<DocumentEngine> engine;
    std::vector<AnnotationRecord> anns;
    std::vector<TocEntry> toc;
    std::map<int, Entry> cache;
    std::map<int, int64_t> wordsOnPage;
    int current = -1;
    int pageCount = 0;
    std::atomic<int64_t> wordPagesCounted{0};

    void emit(RenderEvent e) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(std::move(e));
    }
    bool hasNewWork() {
        std::lock_guard<std::mutex> lock(mutex);
        return stop || open || layout || target.kind != Target::Kind::None || search || annotations || selectRequest ||
               hitRequest || placeRequest || !inkRequests.empty();
    }
    Entry& entry(int page) {
        auto it = cache.find(page);
        if (it != cache.end()) return it->second;
        Entry e;
        e.image = std::make_shared<const PageImage>(engine->render(page));
        e.highlights = engine->highlights(page, anns);
        return cache[page] = std::move(e);
    }
    int64_t words(int page) {
        auto it = wordsOnPage.find(page);
        if (it != wordsOnPage.end()) return it->second;
        ++wordPagesCounted;
        return wordsOnPage[page] = engine->wordsBetween(page, page + 1);
    }
    // Words from `page` to the next chapter: exact for the next pages, estimated beyond.
    int64_t wordsLeft(int page) {
        int next = pageCount;
        for (const auto& t : toc)
            if (t.page > page) {
                next = t.page;
                break;
            }
        const int exactEnd = std::min(next, page + kWordsLookahead);
        int64_t sum = 0;
        for (int p = page; p < exactEnd; ++p) sum += words(p);
        if (next > exactEnd && exactEnd > page) sum += sum * (next - exactEnd) / (exactEnd - page);
        return sum;
    }
    RenderEvent pageEvent(int page) {
        Entry& e = entry(page);
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Page;
        ev.page = page;
        ev.pageCount = pageCount;
        ev.image = e.image;
        ev.highlights = e.highlights;
        ev.chapter = engine->chapterAt(page);
        ev.progress = pageCount > 1 ? static_cast<double>(page) / (pageCount - 1) : 1.0;
        ev.wordsLeftInChapter = wordsLeft(page);
        ev.position = engine->positionOfPage(page);
        ev.zoteroPosition = engine->zoteroPosition(page);
        return ev;
    }
    void relaid(int page) {
        cache.clear();
        wordsOnPage.clear();
        toc = engine->toc();
        std::sort(toc.begin(), toc.end(), [](const TocEntry& a, const TocEntry& b) { return a.page < b.page; });
        pageCount = engine->pageCount();
        current = std::clamp(page, 0, std::max(0, pageCount - 1));
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Opened;
        ev.page = current;
        ev.pageCount = pageCount;
        ev.toc = toc;
        emit(std::move(ev));
    }
    void fail(const char* what) {
        log(LogLevel::Error, "render worker: %s", what);
        engine.reset();
        cache.clear();
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Error;
        ev.error = UserError{ErrorKind::Other, "This book can't be shown (it may be damaged or too large).", what};
        emit(std::move(ev));
    }
    void run();
    void step(bool& postCurrent);
};

void RenderWorker::Impl::run() {
    bool postCurrent = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            const bool prerenderLeft = engine && current >= 0 &&
                                       ((current + 1 < pageCount && !cache.count(current + 1)) ||
                                        (current > 0 && !cache.count(current - 1)));
            if (!postCurrent && !prerenderLeft)
                cv.wait(lock, [&] {
                    return stop || open || layout || target.kind != Target::Kind::None || search || annotations ||
                           selectRequest || hitRequest || placeRequest || !inkRequests.empty();
                });
            if (stop) break;
        }
        try {
            step(postCurrent);
        } catch (const std::exception& e) {
            postCurrent = false;
            fail(e.what());
        } catch (...) {
            postCurrent = false;
            fail("unknown error");
        }
    }
    try {
        engine.reset();  // engine teardown (crengine may write its cache) on this thread, not the UI's
    } catch (...) {
    }
}

void RenderWorker::Impl::step(bool& postCurrent) {
    std::optional<OpenRequest> o;
    std::optional<std::pair<Layout, DocPosition>> l;
    Target t;
    std::optional<std::pair<uint64_t, std::string>> s;
    std::optional<std::vector<AnnotationRecord>> a;
    std::optional<PointRequest> sel, hit;
    std::optional<std::pair<uint64_t, std::string>> place;
    std::deque<InkRequest> inks;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        o = std::move(open);
        open.reset();
        l = std::move(layout);
        layout.reset();
        t = std::move(target);
        target = {};
        s = std::move(search);
        search.reset();
        a = std::move(annotations);
        annotations.reset();
        sel = selectRequest;
        selectRequest.reset();
        hit = hitRequest;
        hitRequest.reset();
        place = std::move(placeRequest);
        placeRequest.reset();
        inks.swap(inkRequests);
        gen = generation;
    }
    if (o) {
        engine = make(o->type);
        if (!engine) {
            RenderEvent ev;
            ev.kind = RenderEvent::Kind::Error;
            ev.error = UserError{ErrorKind::Other, "This book can't be opened.", "no engine"};
            emit(std::move(ev));
            return;
        }
        engine->setLayout(o->layout);
        UserError err;
        if (!engine->open(o->path, &err)) {
            engine.reset();
            RenderEvent ev;
            ev.kind = RenderEvent::Kind::Error;
            ev.error = err;
            emit(std::move(ev));
            return;
        }
        int first = o->start.value.empty() ? 0 : engine->pageOf(o->start);
        if (o->start.value.empty() && !o->zoteroStart.empty())
            if (const int p = engine->pageOfZoteroPosition(o->zoteroStart); p >= 0) first = p;
        relaid(first);
        postCurrent = true;
    }
    if (!engine) return;
    // Ink first: its device points belong to the layout they were drawn on.
    for (auto& k : inks) {
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Ink;
        ev.requestId = k.id;
        ev.page = k.page;
        ev.ink = engine->inkPosition(k.page, k.paths, k.widthPx);
        emit(std::move(ev));
    }
    if (a) {
        anns = std::move(*a);
        for (auto& [p, e] : cache) e.highlights = engine->highlights(p, anns);
        postCurrent = true;
    }
    if (l) {
        engine->setLayout(l->first);
        relaid(l->second.value.empty() ? current : engine->pageOf(l->second));
        postCurrent = true;
    }
    if (t.kind != Target::Kind::None) {
        int page = current;
        if (t.kind == Target::Kind::Page) page = t.page;
        else if (t.kind == Target::Kind::Position) page = engine->pageOf(t.position);
        else if (const int p = engine->pageOfAnnotation(t.annotation); p >= 0) page = p;
        current = std::clamp(page, 0, std::max(0, pageCount - 1));
        postCurrent = true;
    }
    if (sel) {
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Selection;
        ev.requestId = sel->id;
        ev.page = sel->page;
        ev.selection = engine->select(sel->page, sel->from, sel->to);
        emit(std::move(ev));
    }
    if (hit) {
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::HitAnnotation;
        ev.requestId = hit->id;
        ev.page = hit->page;
        ev.annotationKey = engine->annotationAt(hit->page, hit->from, anns).value_or("");
        emit(std::move(ev));
    }
    if (place) {
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Placed;
        ev.requestId = place->first;
        ev.page = engine->pageOfZoteroPosition(place->second);
        ev.pageCount = pageCount;
        if (ev.page >= 0) {
            ev.chapter = engine->chapterAt(ev.page);
            ev.progress = pageCount > 1 ? static_cast<double>(ev.page) / (pageCount - 1) : 1.0;
        }
        emit(std::move(ev));
    }
    if (s) {
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::SearchResults;
        ev.requestId = s->first;
        ev.hits = engine->search(s->second, 100);
        emit(std::move(ev));
    }
    if (postCurrent) {
        RenderEvent ev = pageEvent(current);
        postCurrent = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != gen) return;  // overtaken while rendering: don't post
        }
        emit(std::move(ev));
        for (auto it = cache.begin(); it != cache.end();)
            it = std::abs(it->first - current) > 1 ? cache.erase(it) : std::next(it);
        return;
    }
    // Idle: pre-render the next page, then the previous one, unless something new arrives.
    for (int p : {current + 1, current - 1}) {
        if (hasNewWork()) break;
        if (p >= 0 && p < pageCount && !cache.count(p)) entry(p);
    }
}

RenderWorker::RenderWorker(Factory make) : impl_(std::make_shared<Impl>()) {
    impl_->make = std::move(make);
    {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        ++g_live;
    }
    std::thread([impl = impl_] {
        impl->run();
        {
            std::lock_guard<std::mutex> lock(g_liveMutex);
            --g_live;
        }
        g_liveCv.notify_all();
    }).detach();
}

RenderWorker::~RenderWorker() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    // No join: the thread holds the shared state and exits after its current engine call.
}

void RenderWorker::waitForAll(int timeoutMs) {
    std::unique_lock<std::mutex> lock(g_liveMutex);
    g_liveCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_live == 0; });
}

int64_t RenderWorker::wordPagesCounted() const { return impl_->wordPagesCounted; }

namespace {

// Changes a request under the lock, then wakes the worker.
template <class F> void request(RenderWorker::Impl& impl, F&& change) {
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        change();
    }
    impl.cv.notify_all();
}

}  // namespace

void RenderWorker::open(std::string path, DocType type, Layout layout, DocPosition start, std::string zoteroStart) {
    request(*impl_, [&] {
        impl_->open = Impl::OpenRequest{std::move(path), type, layout, std::move(start), std::move(zoteroStart)};
        impl_->layout.reset();
        impl_->target = {};
        ++impl_->generation;
    });
}

void RenderWorker::setLayout(Layout layout, DocPosition keep) {
    request(*impl_, [&] {
        impl_->layout = std::make_pair(layout, std::move(keep));
        impl_->target = {};
        ++impl_->generation;
    });
}

void RenderWorker::show(int page) {
    request(*impl_, [&] {
        impl_->target = {Impl::Target::Kind::Page, page, {}, {}};
        ++impl_->generation;
    });
}

void RenderWorker::showPosition(DocPosition pos) {
    request(*impl_, [&] {
        impl_->target = {Impl::Target::Kind::Position, 0, std::move(pos), {}};
        ++impl_->generation;
    });
}

void RenderWorker::showAnnotation(AnnotationRecord a) {
    request(*impl_, [&] {
        impl_->target = {Impl::Target::Kind::Annotation, 0, {}, std::move(a)};
        ++impl_->generation;
    });
}

uint64_t RenderWorker::placeZotero(std::string json) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextPointId++;
        impl_->placeRequest = std::make_pair(id, std::move(json));
    });
    return id;
}

uint64_t RenderWorker::search(std::string query) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextSearchId++;
        impl_->search = std::make_pair(id, std::move(query));
    });
    return id;
}

void RenderWorker::highlightsFor(std::vector<AnnotationRecord> anns) {
    request(*impl_, [&] { impl_->annotations = std::move(anns); });
}

uint64_t RenderWorker::select(int page, Point from, Point to) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextPointId++;
        impl_->selectRequest = Impl::PointRequest{id, page, from, to};
    });
    return id;
}

uint64_t RenderWorker::annotationAt(int page, Point p) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextPointId++;
        impl_->hitRequest = Impl::PointRequest{id, page, p, p};
    });
    return id;
}

uint64_t RenderWorker::ink(int page, std::vector<std::vector<Point>> paths, int widthPx) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextPointId++;
        impl_->inkRequests.push_back(Impl::InkRequest{id, page, std::move(paths), widthPx});
    });
    return id;
}

bool RenderWorker::poll(RenderEvent& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->events.empty()) return false;
    out = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

}  // namespace zk
