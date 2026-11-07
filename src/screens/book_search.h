#pragma once
// Search in the open book: a field (system keyboard) and hits with their page and context.
// The search runs on the render worker; tapping a hit returns to the reader at that place.

#include <string>
#include <vector>

#include "docs/document.h"
#include "screens/screen.h"
#include "ui/text_field.h"

namespace zk {

class ReaderScreen;

class BookSearchScreen final : public Screen {
public:
    explicit BookSearchScreen(ReaderScreen&);
    ~BookSearchScreen() override;

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool wantsKeyboard() const override { return true; }
    bool showsTabBar() const override { return false; }
    bool isReading() const override { return true; }  // turns with the reader

    bool searching() const { return searching_; }
    const std::vector<SearchHit>& hits() const { return hits_; }
    std::vector<Rect> hitRects() const;
    std::string emptyMessage() const { return searched_ && hits_.empty() ? "Not found in this book." : ""; }

private:
    void leave(Nav&);

    ReaderScreen& reader_;
    TextField field_;
    std::vector<SearchHit> hits_;
    bool searching_ = false, searched_ = false, gotResults_ = false;
    uint64_t requestId_ = 0;  // results for any other request (an earlier query, another screen) are ignored
    int areaBottom_ = 0;  // set in onShow: the keyboard's top, or the window's bottom
};

}  // namespace zk
