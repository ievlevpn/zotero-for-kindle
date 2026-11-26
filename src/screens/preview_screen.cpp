#include "screens/preview_screen.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/wake.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;
namespace fs = std::filesystem;

namespace {

int W() { return metrics().width; }
constexpr int kDownloadMs = 5 * 60 * 1000;  // a 50 MB PDF on a slow connection

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// "https://arxiv.org/pdf/2401.01234v2" → "2401.01234v2"; the host when the path says nothing.
std::string titleFromUrl(const std::string& url) {
    std::string rest = url.substr(url.find("://") == std::string::npos ? 0 : url.find("://") + 3);
    rest = rest.substr(0, rest.find_first_of("?#"));
    const size_t slash = rest.find('/');
    const std::string host = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash);
    while (!path.empty() && path.back() == '/') path.pop_back();
    const std::string last = path.substr(path.rfind('/') + 1);
    return last.empty() ? host : last;
}

std::string hostPart(const std::string& url) {
    std::string rest = url.substr(url.find("://") == std::string::npos ? 0 : url.find("://") + 3);
    return rest.substr(0, rest.find_first_of("/?#"));
}

// A download's error, without zk.fetch's wording: "The download failed: …", "The response is over 50 MB".
std::string friendly(std::string m) {
    if (m.rfind("zk.fetch(\"", 0) == 0) {
        const size_t end = m.find("\")");
        if (end != std::string::npos) {
            std::string rest = m.substr(end + 2);
            if (rest.rfind(": ", 0) == 0) {
                rest = rest.substr(2);
                if (!rest.empty() && rest[0] >= 'a' && rest[0] <= 'z') rest[0] = static_cast<char>(rest[0] - 'a' + 'A');
                return rest;
            }
            return "The download" + rest;
        }
    }
    return m;
}

std::string mb(int64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::string firstBytes(const std::string& path, size_t n) {
    std::ifstream in(path, std::ios::binary);
    std::string s(n, '\0');
    in.read(s.data(), static_cast<std::streamsize>(n));
    s.resize(static_cast<size_t>(in.gcount()));
    return s;
}

}  // namespace

PreviewScreen::PreviewScreen(PreviewDoc doc, Deps deps) : doc_(std::move(doc)), deps_(std::move(deps)) {
    title_ = !doc_.title.empty() ? doc_.title : titleFromUrl(doc_.url);
}

PreviewScreen::~PreviewScreen() {
    if (shared_) *shared_->cancel = true;
    if (thread_.joinable()) thread_.join();
}

Rect PreviewScreen::progressRect() { return {kMargin, kHeaderH + 360, W() - 2 * kMargin, 140}; }
Rect PreviewScreen::cancelRect() { return {(W() - 400) / 2, kHeaderH + 560, 400, 110}; }

void PreviewScreen::tell(const std::string& error) {
    if (answered_) return;
    answered_ = true;
    if (deps_.answer) deps_.answer(error);
}

std::optional<PreviewEntry> PreviewScreen::ready(const PreviewDoc& doc, PreviewCache& cache, const std::string& title,
                                                 std::string* error) {
    if (!doc.url.empty()) return cache.find(PreviewCache::keyForUrl(doc.url));  // read before: no download, its place kept
    // A page the plugin has: written out, and opened like a web snapshot.
    const std::string key = PreviewCache::keyForHtml(doc.html);
    if (auto e = cache.find(key)) return e;
    const std::string part = cache.partPath(key);
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << previewHtmlPage(doc.html, title, doc.baseUrl);
        if (!out) {
            *error = "Couldn't write the page";
            return std::nullopt;
        }
    }
    return cache.add(key, part, PreviewType::Html, title, error);
}

std::unique_ptr<ReaderScreen> PreviewScreen::reader(const PreviewEntry& e, const PreviewDoc& doc, const Deps& deps) {
    auto r = deps.makeReader ? deps.makeReader(e) : nullptr;
    if (!r) return nullptr;
    PreviewCache* cache = deps.cache;
    const std::string key = e.key;
    r->setPreview({e.position, doc.url, [cache, key](const std::string& pos) { cache->savePosition(key, pos); }, deps.closed,
                   doc.saveMode, deps.closedWithResult});
    return r;
}

void PreviewScreen::show(PreviewDoc doc, Deps deps, Nav& nav) {
    const std::string title = !doc.title.empty() ? doc.title : titleFromUrl(doc.url);
    std::string error;
    if (auto e = ready(doc, *deps.cache, title, &error)) {
        if (auto r = reader(*e, doc, deps)) {
            if (deps.answer) deps.answer("");
            nav.push(std::move(r));
            return;
        }
        error = "This document can't be read here";
    }
    if (!error.empty()) {
        if (deps.answer) deps.answer(error);
        nav.toast(error);
        return;
    }
    nav.push(std::make_unique<PreviewScreen>(std::move(doc), std::move(deps)));
}

void PreviewScreen::onShow(Nav&) {
    if (started_) return;
    started_ = true;
    start();
}

void PreviewScreen::start() {
    PreviewCache& cache = *deps_.cache;
    key_ = PreviewCache::keyForUrl(doc_.url);
    shared_ = std::make_shared<Shared>();
    HttpRequest req;
    req.url = doc_.url;
    req.outFile = cache.partPath(key_);
    req.cancel = shared_->cancel;
    auto sh = shared_;
    auto lastWake = std::make_shared<std::atomic<int64_t>>(0);
    auto lastPercent = std::make_shared<std::atomic<int>>(-1);
    req.onProgress = [sh, lastWake, lastPercent](int64_t done, int64_t total) {
        sh->done = done;
        sh->total = total;
        // The screen redraws when the percentage it shows changes, at most once a second.
        const int pct = total > 0 ? static_cast<int>(100 * done / total) : static_cast<int>(done >> 20);
        const int64_t now = steadyMs();
        if (pct != lastPercent->load() && now - lastWake->load() >= 1000) {
            lastPercent->store(pct);
            lastWake->store(now);
            wakeUi();
        }
    };
    thread_ = std::thread([sh, req = std::move(req), download = deps_.download]() mutable {
        std::optional<HttpResponse> resp;
        std::string error;
        try {
            resp = download(std::move(req));
        } catch (const std::exception& e) {
            error = friendly(e.what());
        }
        {
            std::lock_guard<std::mutex> lock(sh->mu);
            sh->response = std::move(resp);
            sh->error = std::move(error);
        }
        sh->finished = true;
        wakeUi();
    });
}

void PreviewScreen::onTick(Nav& nav) {
    if (!shared_ || answered_) return;
    if (finished()) return finish(nav);
    const int64_t done = shared_->done, total = shared_->total;
    const int pct = total > 0 ? static_cast<int>(100 * done / total) : -1;
    if (pct != shownPercent_ || (total <= 0 && done - shownDone_ >= (1 << 20))) {
        shownPercent_ = pct;
        shownDone_ = done;
        nav.damage(progressRect(), Waveform::Ui);
    }
}

void PreviewScreen::finish(Nav& nav) {
    if (thread_.joinable()) thread_.join();
    std::optional<HttpResponse> resp;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(shared_->mu);
        resp = std::move(shared_->response);
        error = shared_->error;
    }
    PreviewCache& cache = *deps_.cache;
    const std::string part = cache.partPath(key_);
    std::error_code ec;
    if (*shared_->cancel) return;  // cancel() answered already
    if (!error.empty() || !resp) {
        fs::remove(part, ec);
        return fail(error.empty() ? "The download failed" : error, nav);
    }
    if (resp->status < 200 || resp->status >= 300) {
        fs::remove(part, ec);
        return fail("The server answered " + std::to_string(resp->status), nav);
    }
    const auto ct = resp->headers.find("content-type");
    const PreviewType type = sniffPreviewType(ct == resp->headers.end() ? "" : ct->second, firstBytes(part, 1024));
    if (type == PreviewType::Unknown) {
        fs::remove(part, ec);
        return fail("This isn't a PDF, EPUB or web page", nav);
    }
    if (type == PreviewType::Html) {  // a page: opened like a web snapshot, its links resolved against where it came from
        std::ostringstream ss;
        ss << std::ifstream(part, std::ios::binary).rdbuf();
        std::ofstream(part, std::ios::binary | std::ios::trunc) << previewHtmlPage(ss.str(), title_, resp->url.empty() ? doc_.url : resp->url);
    }
    std::string err;
    auto e = cache.add(key_, part, type, title_, &err);
    if (!e) return fail(err, nav);
    open(*e, nav);
}

void PreviewScreen::open(const PreviewEntry& e, Nav& nav) {
    auto r = reader(e, doc_, deps_);
    if (!r) return fail("This document can't be read here", nav);
    tell("");
    nav.replaceTop(std::move(r));  // this screen is gone: nothing after this line
}

void PreviewScreen::fail(const std::string& error, Nav& nav) {
    tell(error);
    nav.toast(error);
    nav.pop();
}

void PreviewScreen::cancel(Nav& nav) {
    if (shared_) *shared_->cancel = true;
    tell("Cancelled");
    nav.pop();
}

void PreviewScreen::onClose(Nav&) {
    if (shared_) *shared_->cancel = true;
    tell("Cancelled");
}

void PreviewScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos) || cancelRect().contains(g.pos)) cancel(nav);
}

void PreviewScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), metrics().height}, 255);
    drawHeader(cr, "Preview", true);
    drawText(cr, title_, {kSerif, 11, 700}, {kMargin, kHeaderH + 40, W() - 2 * kMargin, 220}, 3);
    if (!doc_.url.empty())
        drawText(cr, hostPart(doc_.url), {kSans, 7.5, 400, false, kSecondary}, {kMargin, kHeaderH + 270, W() - 2 * kMargin, 60}, 1);
    const Rect p = progressRect();
    const int64_t done = shared_ ? shared_->done.load() : 0, total = shared_ ? shared_->total.load() : 0;
    progressBar(cr, {p.x, p.y + 20, p.w, 12}, total > 0 ? static_cast<double>(done) / total : 0);
    const std::string line = total > 0 ? "Downloading · " + mb(done) + " of " + mb(total)
                                       : done > 0 ? "Downloading · " + mb(done) : "Downloading…";
    drawText(cr, line, {kSans, 7.5, 400, false, kSecondary}, {p.x, p.y + 60, p.w, 60}, 1);
    drawButton(cr, cancelRect(), "Cancel", false);
}

}  // namespace zk
