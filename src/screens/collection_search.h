#pragma once
// Collection search (from the Collections header): find a collection at any depth by part of
// its name, any case (Cyrillic too); each result shows its path; tapping one opens it, a long press
// shows the collection's sheet (Pin to top, Keep downloaded) as in the Collections tab. Results
// that don't fit above the keyboard are on further pages.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "screens/collections.h"
#include "screens/screen.h"
#include "ui/text_field.h"

namespace zk {

class CollectionSearchScreen final : public Screen {
public:
    struct Result {
        CollectionEntry entry;
        std::string path;  // "Economics › Labour markets"
    };
    static constexpr int64_t kPauseMs = 700;

    explicit CollectionSearchScreen(std::function<void(const ItemRow&)> openItem, StatusFn status = {});
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool wantsKeyboard() const override { return true; }
    bool showsTabBar() const override { return false; }

    const std::vector<Result>& results() const { return shown_; }
    // More results than fit above the keyboard: pages (swipe up and down), "2 / 3" under them.
    std::vector<Rect> resultRects() const;  // this page's
    size_t firstShown() const { return static_cast<size_t>(page_ * perPage()); }  // the result in resultRects()[0]
    int page() const { return page_; }
    int pages() const;
    const PinSheet* pinSheet() const { return sheet_ ? &*sheet_ : nullptr; }  // a long press on a result

private:
    void search(Nav&);
    void leave(Nav&);
    int perPage() const;
    Rect indicatorRect() const;

    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    TextField field_;
    std::vector<Result> shown_;
    std::string searched_;
    bool pending_ = false;
    int64_t lastKeyMs_ = 0;
    int bottom_;  // above the keyboard (the window's bottom without one)
    int page_ = 0;
    std::optional<PinSheet> sheet_;
};

}  // namespace zk
