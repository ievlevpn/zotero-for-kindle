#pragma once
// Collection search (from the Collections header): find a collection at any depth by part of
// its name, any case (Cyrillic too); each result shows its path; tapping one opens it.

#include <functional>
#include <string>
#include <vector>

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
    std::vector<Rect> resultRects() const;

private:
    void search(Nav&);
    void leave(Nav&);

    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    TextField field_;
    std::vector<Result> shown_;
    std::string searched_;
    bool pending_ = false;
    int64_t lastKeyMs_ = 0;
    int bottom_ = 1547;  // above the keyboard
};

}  // namespace zk
