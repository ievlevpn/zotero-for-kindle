#pragma once
// A book's table of contents and its bookmarks (two tabs), and Go to…; tapping an entry goes there.

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
    std::vector<Rect> entryRects() const;  // of the tab shown
    void setBookmarks(std::vector<TocEntry>);    // the Bookmarks tab (shown once set)
    void setGoTo(std::function<void(Nav&)>);     // the Go to… button
    static Rect tabRect(int i);                  // 0 Contents, 1 Bookmarks
    static Rect goToRect();

private:
    const std::vector<TocEntry>& shown() const { return tab_ == 1 ? marks_ : toc_; }
    Rect contentArea() const;
    int perPage() const;
    std::vector<TocEntry> toc_, marks_;
    bool tabs_ = false;
    int tab_ = 0;
    std::function<void(Nav&)> goTo_;
    int current_;
    std::function<void(const TocEntry&)> pick_;
    Pager pager_{0, 1};
};

}  // namespace zk
