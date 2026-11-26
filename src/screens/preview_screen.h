#pragma once
// Opening a preview (zk.reader.open, 0.9): an `html` document, or one already in the preview cache,
// opens in the reader at once; a `url` is downloaded first on this screen — its title, the bytes
// so far and Cancel — then the reader takes its place. The download runs on a thread of its own
// through the plugin's net rules (PluginHost::previewDownload, the app's HTTP stack); the UI is
// woken only when the shown percentage changes (at most once a second) and when it ends.
// `answer` gets "" once the reader shows, else why not ("Cancelled", a download's error); a failure
// also leaves a toast and goes back. `closed` runs when that reader closes.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "net/http.h"
#include "screens/preview.h"
#include "screens/reader.h"
#include "screens/screen.h"

namespace zk {

struct PreviewDoc {
    std::string url;   // a PDF, an EPUB, or a page (opened as html)
    std::string html;  // or the page itself
    std::string title, baseUrl;
    ReaderScreen::PreviewSaveMode saveMode = ReaderScreen::PreviewSaveMode::None;
};

class PreviewScreen final : public Screen {
public:
    struct Deps {
        PreviewCache* cache = nullptr;
        // Downloads req.url into req.outFile (onProgress, cancel set); throws std::exception.
        std::function<HttpResponse(HttpRequest)> download;
        // The reader for a cached document (nullptr: it can't be read here).
        std::function<std::unique_ptr<ReaderScreen>(const PreviewEntry&)> makeReader;
        std::function<void(const std::string& error)> answer;
        std::function<void()> closed;
        std::function<void(ReaderScreen::PreviewCloseResult)> closedWithResult;
    };
    // Opens `doc`: the reader at once for html or a cached document, else this screen over what
    // shows, downloading. The way in (PreviewScreen's constructor is for tests).
    static void show(PreviewDoc, Deps, Nav&);

    PreviewScreen(PreviewDoc, Deps);
    ~PreviewScreen() override;

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    void onClose(Nav&) override;
    bool showsTabBar() const override { return false; }

    static Rect cancelRect();
    static Rect progressRect();  // the bar and the "2.1 of 3.4 MB" under it
    bool downloading() const { return thread_.joinable() && !finished(); }
    const std::string& title() const { return title_; }

private:
    struct Shared {
        std::atomic<int64_t> done{0}, total{0};
        std::atomic<bool> finished{false};
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::mutex mu;
        std::optional<HttpResponse> response;
        std::string error;
    };
    bool finished() const { return shared_ && shared_->finished.load(); }
    void start();
    void finish(Nav&);                       // the download ended
    void open(const PreviewEntry&, Nav&);    // the reader takes this screen's place
    // The cached document (html written out first) to open at once, or nullopt: download it.
    // *error when the page couldn't be written.
    static std::optional<PreviewEntry> ready(const PreviewDoc&, PreviewCache&, const std::string& title, std::string* error);
    static std::unique_ptr<ReaderScreen> reader(const PreviewEntry&, const PreviewDoc&, const Deps&);
    void fail(const std::string& error, Nav&);
    void cancel(Nav&);
    void tell(const std::string& error);     // answers once

    PreviewDoc doc_;
    Deps deps_;
    std::string title_, key_;
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    int shownPercent_ = -1;
    int64_t shownDone_ = 0;
    bool started_ = false, answered_ = false;
};

}  // namespace zk
