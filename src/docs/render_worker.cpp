#include "docs/render_worker.h"

#include <algorithm>

namespace zk {

RenderWorker::RenderWorker(Factory make) : make_(std::move(make)) { thread_ = std::thread([this] { run(); }); }

RenderWorker::~RenderWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void RenderWorker::open(std::string path, DocType type, Layout layout, DocPosition start) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = OpenRequest{std::move(path), type, layout, std::move(start)};
        layout_.reset();
        target_ = {};
        ++generation_;
    }
    cv_.notify_all();
}

void RenderWorker::setLayout(Layout layout, DocPosition keep) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        layout_ = std::make_pair(layout, std::move(keep));
        target_ = {};
        ++generation_;
    }
    cv_.notify_all();
}

void RenderWorker::show(int page) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_ = {Target::Kind::Page, page, {}, {}};
        ++generation_;
    }
    cv_.notify_all();
}

void RenderWorker::showPosition(DocPosition pos) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_ = {Target::Kind::Position, 0, std::move(pos), {}};
        ++generation_;
    }
    cv_.notify_all();
}

void RenderWorker::showAnnotation(AnnotationRecord a) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_ = {Target::Kind::Annotation, 0, {}, std::move(a)};
        ++generation_;
    }
    cv_.notify_all();
}

void RenderWorker::search(std::string query) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        search_ = std::move(query);
    }
    cv_.notify_all();
}

void RenderWorker::highlightsFor(std::vector<AnnotationRecord> anns) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        annotations_ = std::move(anns);
    }
    cv_.notify_all();
}

bool RenderWorker::poll(RenderEvent& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

void RenderWorker::emit(RenderEvent e) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(e));
}

bool RenderWorker::hasNewWork() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_ || open_ || layout_ || target_.kind != Target::Kind::None || search_ || annotations_;
}

RenderWorker::Entry& RenderWorker::entry(int page) {
    auto it = cache_.find(page);
    if (it != cache_.end()) return it->second;
    Entry e;
    e.image = std::make_shared<const PageImage>(engine_->render(page));
    e.highlights = engine_->highlights(page, anns_);
    return cache_[page] = std::move(e);
}

RenderEvent RenderWorker::pageEvent(int page) {
    Entry& e = entry(page);
    RenderEvent ev;
    ev.kind = RenderEvent::Kind::Page;
    ev.page = page;
    ev.pageCount = pageCount_;
    ev.image = e.image;
    ev.highlights = e.highlights;
    ev.chapter = engine_->chapterAt(page);
    ev.progress = pageCount_ > 1 ? static_cast<double>(page) / (pageCount_ - 1) : 1.0;
    int nextChapter = pageCount_;
    for (const auto& t : toc_)
        if (t.page > page) {
            nextChapter = t.page;
            break;
        }
    ev.wordsLeftInChapter = engine_->wordsBetween(page, nextChapter);
    ev.position = engine_->positionOfPage(page);
    return ev;
}

// After open or re-pagination: tell the reader, and make `page` current.
void RenderWorker::relaid(int page) {
    cache_.clear();
    toc_ = engine_->toc();
    std::sort(toc_.begin(), toc_.end(), [](const TocEntry& a, const TocEntry& b) { return a.page < b.page; });
    pageCount_ = engine_->pageCount();
    current_ = std::clamp(page, 0, std::max(0, pageCount_ - 1));
    RenderEvent ev;
    ev.kind = RenderEvent::Kind::Opened;
    ev.page = current_;
    ev.pageCount = pageCount_;
    ev.toc = engine_->toc();
    emit(std::move(ev));
}

void RenderWorker::run() {
    bool postCurrent = false;
    for (;;) {
        std::optional<OpenRequest> open;
        std::optional<std::pair<Layout, DocPosition>> layout;
        Target target;
        std::optional<std::string> search;
        std::optional<std::vector<AnnotationRecord>> anns;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool prerenderLeft =
                engine_ && current_ >= 0 && ((current_ + 1 < pageCount_ && !cache_.count(current_ + 1)) ||
                                             (current_ > 0 && !cache_.count(current_ - 1)));
            if (!postCurrent && !prerenderLeft)
                cv_.wait(lock, [&] {
                    return stop_ || open_ || layout_ || target_.kind != Target::Kind::None || search_ || annotations_;
                });
            if (stop_) return;
            open = std::move(open_);
            open_.reset();
            layout = std::move(layout_);
            layout_.reset();
            target = std::move(target_);
            target_ = {};
            search = std::move(search_);
            search_.reset();
            anns = std::move(annotations_);
            annotations_.reset();
            generation = generation_;
        }

        if (open) {
            engine_ = make_(open->type);
            UserError err;
            if (!engine_) {
                emit({RenderEvent::Kind::Error, 0, 0, nullptr, {}, {}, 0, 0, {}, {},
                      UserError{ErrorKind::Other, "This book can't be opened.", "no engine"}, {}});
                continue;
            }
            engine_->setLayout(open->layout);
            if (!engine_->open(open->path, &err)) {
                engine_.reset();
                RenderEvent ev;
                ev.kind = RenderEvent::Kind::Error;
                ev.error = err;
                emit(std::move(ev));
                continue;
            }
            relaid(open->start.value.empty() ? 0 : engine_->pageOf(open->start));
            postCurrent = true;
        }
        if (!engine_) continue;
        if (anns) {
            anns_ = std::move(*anns);
            for (auto& [p, e] : cache_) e.highlights = engine_->highlights(p, anns_);
            postCurrent = true;  // repaint with the new highlights
        }
        if (layout) {
            engine_->setLayout(layout->first);
            relaid(engine_->pageOf(layout->second));
            postCurrent = true;
        }
        if (target.kind != Target::Kind::None) {
            int page = current_;
            if (target.kind == Target::Kind::Page) page = target.page;
            else if (target.kind == Target::Kind::Position) page = engine_->pageOf(target.position);
            else if (const int p = engine_->pageOfAnnotation(target.annotation); p >= 0) page = p;
            current_ = std::clamp(page, 0, std::max(0, pageCount_ - 1));
            postCurrent = true;
        }
        if (search) {
            RenderEvent ev;
            ev.kind = RenderEvent::Kind::SearchResults;
            ev.hits = engine_->search(*search, 100);
            emit(std::move(ev));
        }
        if (postCurrent) {
            RenderEvent ev = pageEvent(current_);
            postCurrent = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation_ != generation) continue;  // overtaken while rendering: don't post
            }
            emit(std::move(ev));
            // Keep only the neighbourhood.
            for (auto it = cache_.begin(); it != cache_.end();)
                it = std::abs(it->first - current_) > 1 ? cache_.erase(it) : std::next(it);
            continue;
        }
        // Idle: pre-render the next page, then the previous one, unless something new arrives.
        for (int p : {current_ + 1, current_ - 1}) {
            if (hasNewWork()) break;
            if (p >= 0 && p < pageCount_ && !cache_.count(p)) entry(p);
        }
    }
}

}  // namespace zk
