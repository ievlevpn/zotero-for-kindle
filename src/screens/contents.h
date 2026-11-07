#pragma once
// A book's table of contents; tapping an entry goes there.

#include <functional>
#include <vector>

#include "docs/document.h"
#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class ContentsScreen final : public Screen {
public:
    ContentsScreen(std::vector<TocEntry> toc, int currentPage, std::function<void(const TocEntry&)> pick);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }
    bool isReading() const override { return true; }  // turns with the reader
    std::vector<Rect> entryRects() const;

private:
    std::vector<TocEntry> toc_;
    int current_;
    std::function<void(const TocEntry&)> pick_;
    Pager pager_{0, 1};
};

}  // namespace zk
