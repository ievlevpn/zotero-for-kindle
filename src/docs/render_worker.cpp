#include "docs/render_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>

#include "core/log.h"
#include "core/wake.h"

namespace zk {

namespace {

constexpr int kWordsLookahead = 30;  // pages counted exactly for "time left"; the rest estimated

// Threads still running (their readers may be gone): main waits for them before exiting.
std::mutex g_liveMutex;
std::condition_variable g_liveCv;
int g_live = 0;

}  // namespace

size_t pageCacheBudget(size_t pageBytes, bool gray) {
    // Gray: four pages at most (a small screen's pages don't pile up to the big screen's budget).
    const size_t base = gray ? std::min(kGrayPageCacheBytes, 4 * pageBytes) : kPageCacheBytes;
    return std::max(base, 2 * pageBytes);
}

std::vector<int> pagesToKeep(int current, int pageCount, size_t pageBytes, bool gray) {
    std::vector<int> out{current};
    size_t used = pageBytes;
    for (int p : {current + 1, current - 1, current + 2}) {
        if (p < 0 || p >= pageCount) continue;
        if (used + pageBytes > pageCacheBudget(pageBytes, gray)) break;
        used += pageBytes;
        out.push_back(p);
    }
    return out;
}

void boostSaturation(uint8_t* rgb, size_t px, int pct) {
    if (pct <= 0) return;
    const float k = 1.0f + pct / 100.0f;
    for (uint8_t* p = rgb; p < rgb + px * 3; p += 3) {
        if (p[0] == p[1] && p[1] == p[2]) continue;  // gray: exactly as it was
        const float r = p[0], g = p[1], b = p[2];
        const float hsp = std::sqrt(0.299f * r * r + 0.587f * g * g + 0.114f * b * b);  // perceived brightness
        for (int i = 0; i < 3; ++i) p[i] = static_cast<uint8_t>(std::clamp(hsp + (p[i] - hsp) * k + 0.5f, 0.0f, 255.0f));
    }
}

int colorBoostPct(const char* env) {
    if (!env || !*env) return 25;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (*end) return 25;
    return static_cast<int>(std::clamp(v, 0L, 100L));
}

struct RenderWorker::Impl {
    struct Target {
        enum class Kind { None, Page, Position, Annotation, Zotero } kind = Kind::None;
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
        bool color = false;
        int boostPct = 0;
    };
    struct Entry {
        std::shared_ptr<const PageImage> image;
        std::vector<HighlightBox> highlights;
        // Found with the image (pre-rendered too), so posting a page asks the engine little.
        std::vector<LinkBox> links;
        Rect pageBox;
    };

    Factory make;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    // Requests (guarded by mutex).
    std::optional<OpenRequest> open;
    std::optional<std::pair<Layout, DocPosition>> layout;
    uint64_t layoutRequestId = 0, nextLayoutId = 1;  // the pending setLayout's id; the next one's
    uint64_t layoutId = 0;                           // the layout in effect (worker thread)
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
    std::optional<std::vector<DocPosition>> bookmarkRequest;
    std::optional<std::tuple<uint64_t, int, int>> linkRequest;  // id, page, index
    uint64_t nextPointId = 1;
    struct InkRequest {
        uint64_t id = 0;
        int page = 0;
        std::vector<std::vector<Point>> paths;
        int widthPx = 0;
        std::optional<Rect> area;   // an area instead of strokes
        std::optional<Point> note;  // a sticky note instead
    };
    std::deque<InkRequest> inkRequests;
    bool trimRequest = false;
    std::optional<std::pair<bool, int>> colorRequest;  // Display → Color: on, boost
    std::deque<RenderEvent> events;
    // Worker-only state.
    std::unique_ptr<DocumentEngine> engine;
    int boostPct = 0;  // color pages' saturation boost
    bool colorPages = false;  // the engine draws RGB pages (set with it, not read off an image: a
                              // failed render's image is empty, neither gray nor of any size)
    // The size of the pages kept (all of a document's are the same): the largest image in the cache.
    size_t keptPageBytes() const {
        size_t each = 0;
        for (const auto& [p, e] : cache) each = std::max(each, pageBytes(*e.image));
        return each;
    }
    std::vector<AnnotationRecord> anns;
    std::vector<DocPosition> bookmarks;
    std::vector<std::pair<std::string, int>> bookmarkPlaces;  // of `bookmarks`, in the current layout
    void placeBookmarks() {
        bookmarkPlaces.clear();
        for (const auto& b : bookmarks) bookmarkPlaces.emplace_back(b.value, b.value.empty() ? -1 : engine->pageOfPlaced(b));
    }
    std::vector<TocEntry> toc;
    std::map<int, Entry> cache;
    std::map<int, int64_t> wordsOnPage;
    int current = -1;
    int pageCount = 0;
    std::atomic<int64_t> wordPagesCounted{0};
    std::atomic<size_t> peakCachedBytes{0};
    // Before a page is rendered: the pages furthest from the current one go until the cache and the
    // new page (sized as the cached ones) fit the budget, so a burst of turns or jumps (renders
    // overtaken, never posted) can't pile pages up.
    void makeRoom() {
        if (cache.empty()) return;
        const size_t each = keptPageBytes();
        const size_t budget = pageCacheBudget(each, !colorPages);
        size_t used = 0;
        for (const auto& [p, e] : cache) used += pageBytes(*e.image);
        while (!cache.empty() && used + each > budget) {
            auto far = std::max_element(cache.begin(), cache.end(), [&](const auto& a, const auto& b) {
                return std::abs(a.first - current) < std::abs(b.first - current);
            });
            used -= pageBytes(*far->second.image);
            cache.erase(far);
        }
    }
    void notePeak() {
        size_t sum = 0;
        for (const auto& [p, e] : cache) sum += pageBytes(*e.image);
        if (sum > peakCachedBytes) peakCachedBytes = sum;
    }

    void emit(RenderEvent e) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(std::move(e));
        }
        wakeUi();  // the reader polls its events only when the loop runs
    }
    bool hasNewWork() {
        std::lock_guard<std::mutex> lock(mutex);
        return stop || open || layout || target.kind != Target::Kind::None || search || annotations || selectRequest ||
               hitRequest || placeRequest || !inkRequests.empty() || bookmarkRequest || linkRequest || trimRequest ||
               colorRequest;
    }
    Entry& entry(int page) {
        auto it = cache.find(page);
        if (it != cache.end()) return it->second;
        makeRoom();
        Entry e;
        PageImage img = engine->render(page);
        if (!img.rgb.empty()) boostSaturation(img.rgb.data(), img.rgb.size() / 3, boostPct);
        e.image = std::make_shared<const PageImage>(std::move(img));
        e.highlights = engine->highlights(page, anns);
        e.links = engine->links(page);
        e.pageBox = engine->pageBox(page);
        Entry& kept = cache[page] = std::move(e);
        notePeak();
        return kept;
    }
    // The pages pagesToKeep wants around the current one (sized as the current page) not rendered yet.
    std::vector<int> toPrerender() {
        std::vector<int> out;
        const auto it = cache.find(current);
        if (it == cache.end()) return out;
        for (int p : pagesToKeep(current, pageCount, keptPageBytes(), !colorPages))
            if (!cache.count(p)) out.push_back(p);
        return out;
    }
    int64_t words(int page) {
        auto it = wordsOnPage.find(page);
        if (it != wordsOnPage.end()) return it->second;
        ++wordPagesCounted;
        return wordsOnPage[page] = engine->wordsBetween(page, page + 1);
    }
    // The pages `page`'s time left counts exactly: up to the next chapter, at most kWordsLookahead.
    std::pair<int, int> wordsWindow(int page) {
        int next = pageCount;
        for (const auto& t : toc)
            if (t.page > page) {
                next = t.page;
                break;
            }
        return {std::min(next, page + kWordsLookahead), next};
    }
    // Words from `page` to the next chapter: the counted pages of the window, scaled to the whole
    // chapter. `count`: count the window's pages not counted yet (else only `page` itself is).
    int64_t wordsLeft(int page, bool count) {
        const auto [exactEnd, next] = wordsWindow(page);
        if (exactEnd <= page) return 0;
        words(page);
        int64_t sum = 0;
        int counted = 0;
        for (int p = page; p < exactEnd; ++p) {
            if (!count && !wordsOnPage.count(p)) continue;
            sum += words(p);
            ++counted;
        }
        return counted == exactEnd - page ? sum + sum * (next - exactEnd) / (exactEnd - page) : sum * (next - page) / counted;
    }
    // Counts the window's next uncounted page (false: none left, or the page changed meanwhile).
    bool countAhead() {
        if (current < 0 || !engine) return false;
        const int exactEnd = wordsWindow(current).first;
        for (int p = current; p < exactEnd; ++p)
            if (!wordsOnPage.count(p)) {
                words(p);
                return true;
            }
        return false;
    }
    int postedWordsPage = -1;  // the page a Page or Words event last gave the time left of, and that
    int64_t postedWords = 0;   // time left
    RenderEvent pageEvent(int page) {
        Entry& e = entry(page);
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Page;
        ev.page = page;
        ev.pageCount = pageCount;
        ev.viewsPerPage = engine->viewsPerPage();
        ev.image = e.image;
        ev.highlights = e.highlights;
        ev.chapter = engine->chapterAt(page);
        ev.progress = pageCount > 1 ? static_cast<double>(page) / (pageCount - 1) : 1.0;
        ev.wordsLeftInChapter = wordsLeft(page, false);  // the rest while idle: not before the page
        postedWordsPage = page;
        postedWords = ev.wordsLeftInChapter;
        ev.position = engine->positionOfPage(page);
        ev.zoteroPosition = engine->zoteroPosition(page);
        ev.bookmarkPlaces = bookmarkPlaces;
        ev.links = e.links;
        ev.pageBox = e.pageBox;
        return ev;
    }
    void relaid(int page) {
        cache.clear();
        wordsOnPage.clear();
        postedWordsPage = -1;
        toc = engine->toc();
        std::sort(toc.begin(), toc.end(), [](const TocEntry& a, const TocEntry& b) { return a.page < b.page; });
        pageCount = engine->pageCount();
        current = std::clamp(page, 0, std::max(0, pageCount - 1));
        placeBookmarks();  // pages move with the layout
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Opened;
        ev.page = current;
        ev.pageCount = pageCount;
        ev.viewsPerPage = engine->viewsPerPage();
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
    // The current page's time left is still to be counted (and posted if it changes).
    bool wordsDue() const { return postedWordsPage >= 0 && postedWordsPage == current; }
    void run();
    void step(bool& postCurrent);
};

void RenderWorker::Impl::run() {
    bool postCurrent = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            const bool prerenderLeft = engine && current >= 0 && (!toPrerender().empty() || wordsDue());
            if (!postCurrent && !prerenderLeft)
                cv.wait(lock, [&] {
                    return stop || open || layout || target.kind != Target::Kind::None || search || annotations ||
                           selectRequest || hitRequest || placeRequest || !inkRequests.empty() || bookmarkRequest || linkRequest ||
                           trimRequest || colorRequest;
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
    uint64_t lId = 0;
    Target t;
    std::optional<std::pair<uint64_t, std::string>> s;
    std::optional<std::vector<AnnotationRecord>> a;
    std::optional<PointRequest> sel, hit;
    std::optional<std::pair<uint64_t, std::string>> place;
    std::optional<std::vector<DocPosition>> marks;
    std::optional<std::tuple<uint64_t, int, int>> linkAsk;
    bool landed = false;
    bool trim = false;
    std::optional<std::pair<bool, int>> color;
    std::deque<InkRequest> inks;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        o = std::move(open);
        open.reset();
        l = std::move(layout);
        layout.reset();
        lId = layoutRequestId;
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
        marks = std::move(bookmarkRequest);
        bookmarkRequest.reset();
        linkAsk = linkRequest;
        linkRequest.reset();
        inks.swap(inkRequests);
        trim = trimRequest;
        trimRequest = false;
        color = colorRequest;
        colorRequest.reset();
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
        engine->setColor(o->color);
        colorPages = o->color;
        boostPct = o->boostPct;
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
    if (trim) engine->trimCaches();
    if (color) {  // every page again, in color or gray
        engine->setColor(color->first);
        colorPages = color->first;
        boostPct = color->second;
        cache.clear();
        postCurrent = true;
    }
    if (marks) {
        bookmarks = std::move(*marks);
        placeBookmarks();
        postCurrent = true;
    }
    // Ink first: its device points belong to the layout they were drawn on.
    for (auto& k : inks) {
        RenderEvent ev;
        ev.kind = k.area || k.note ? RenderEvent::Kind::Area : RenderEvent::Kind::Ink;
        ev.requestId = k.id;
        ev.page = k.page;
        ev.ink = k.note   ? engine->notePosition(k.page, *k.note)
                 : k.area ? engine->areaPosition(k.page, *k.area)
                          : engine->inkPosition(k.page, k.paths, k.widthPx);
        emit(std::move(ev));
    }
    if (a) {
        anns = std::move(*a);
        for (auto& [p, e] : cache) e.highlights = engine->highlights(p, anns);
        postCurrent = true;
    }
    if (linkAsk) {  // before a new layout: the link's index is counted on the page it was tapped on
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Link;
        ev.requestId = std::get<0>(*linkAsk);
        ev.page = std::get<1>(*linkAsk);
        ev.link = engine->link(ev.page, std::get<2>(*linkAsk));
        emit(std::move(ev));
    }
    if (l) {
        engine->setLayout(l->first);
        layoutId = lId;
        relaid(l->second.value.empty() ? current : engine->pageOf(l->second));
        postCurrent = true;
    }
    if (t.kind != Target::Kind::None) {
        int page = current;
        if (t.kind == Target::Kind::Page) page = t.page;
        else if (t.kind == Target::Kind::Position) page = engine->pageOf(t.position);
        else if (t.kind == Target::Kind::Zotero) {
            if (const int p = engine->pageOfZoteroPosition(t.position.value); p >= 0) page = p;
        }
        else if (const int p = engine->pageOfAnnotation(t.annotation); p >= 0) page = p;
        current = std::clamp(page, 0, std::max(0, pageCount - 1));
        postCurrent = true;
        landed = true;
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
        ev.viewsPerPage = engine->viewsPerPage();
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
        ev.landed = landed;
        ev.layoutId = layoutId;
        postCurrent = false;
        // Kept: what fits around the current page (pagesToKeep); the rest goes, posted or not.
        const std::vector<int> keep = pagesToKeep(current, pageCount, keptPageBytes(), !colorPages);
        for (auto it = cache.begin(); it != cache.end();)
            it = std::find(keep.begin(), keep.end(), it->first) == keep.end() ? cache.erase(it) : std::next(it);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != gen) return;  // overtaken while rendering: don't post
        }
        emit(std::move(ev));
        return;
    }
    // Idle: pre-render the next page, then the previous one (as fits), unless something new arrives.
    for (int p : toPrerender()) {
        if (hasNewWork()) return;
        entry(p);
    }
    // Then the time left: the window's pages one at a time, the corrected value posted once all are.
    while (!hasNewWork() && countAhead()) {
    }
    if (hasNewWork() || postedWordsPage != current) return;
    const int64_t exact = wordsLeft(current, true);
    if (exact != postedWords) {
        RenderEvent ev;
        ev.kind = RenderEvent::Kind::Words;
        ev.page = current;
        ev.wordsLeftInChapter = exact;
        emit(std::move(ev));
    }
    postedWords = exact;
    postedWordsPage = -2;  // done for this page
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
size_t RenderWorker::peakCachedBytes() const { return impl_->peakCachedBytes; }

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

void RenderWorker::setColor(bool on, int boostPct) {
    request(*impl_, [&] { impl_->colorRequest = std::make_pair(on, boostPct); });
}

void RenderWorker::open(std::string path, DocType type, Layout layout, DocPosition start, std::string zoteroStart, bool color,
                        int boostPct) {
    request(*impl_, [&] {
        impl_->open = Impl::OpenRequest{std::move(path), type, layout, std::move(start), std::move(zoteroStart), color, boostPct};
        impl_->colorRequest.reset();
        impl_->layout.reset();
        impl_->target = {};
        ++impl_->generation;
    });
}

uint64_t RenderWorker::setLayout(Layout layout, DocPosition keep) {
    uint64_t id = 0;
    request(*impl_, [&] {
        impl_->layout = std::make_pair(layout, std::move(keep));
        impl_->layoutRequestId = id = impl_->nextLayoutId++;
        impl_->target = {};
        ++impl_->generation;
    });
    return id;
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

void RenderWorker::showZotero(std::string position) {
    request(*impl_, [&] {
        impl_->target = {Impl::Target::Kind::Zotero, 0, {std::move(position)}, {}};
        ++impl_->generation;
    });
}

void RenderWorker::showAnnotation(AnnotationRecord a) {
    request(*impl_, [&] {
        impl_->target = {Impl::Target::Kind::Annotation, 0, {}, std::move(a)};
        ++impl_->generation;
    });
}

uint64_t RenderWorker::linkAt(int page, int index) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextPointId++;
        impl_->linkRequest = std::make_tuple(id, page, index);
    });
    return id;
}

void RenderWorker::trimCaches() {
    request(*impl_, [&] { impl_->trimRequest = true; });
}

void RenderWorker::setBookmarks(std::vector<DocPosition> marks) {
    request(*impl_, [&] { impl_->bookmarkRequest = std::move(marks); });
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

uint64_t RenderWorker::area(int page, Rect r) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextPointId++;
        impl_->inkRequests.push_back(Impl::InkRequest{id, page, {}, 0, r});
    });
    return id;
}

uint64_t RenderWorker::note(int page, Point at) {
    uint64_t id = 0;
    request(*impl_, [&] {
        id = impl_->nextPointId++;
        impl_->inkRequests.push_back(Impl::InkRequest{id, page, {}, 0, std::nullopt, at});
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
