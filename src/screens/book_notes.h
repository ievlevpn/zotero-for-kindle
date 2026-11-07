#pragma once
// The highlights and notes of the open book; tapping one goes to its page.

#include <functional>

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class BookNotesScreen final : public Screen {
public:
    BookNotesScreen(ItemRow row, std::function<void(const AnnotationRecord&)> pick);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }
    std::vector<Rect> rowRects() const;

private:
    ItemRow row_;
    std::function<void(const AnnotationRecord&)> pick_;
    std::vector<AnnotationRecord> all_;
    Pager pager_{0, 1};
};

}  // namespace zk
