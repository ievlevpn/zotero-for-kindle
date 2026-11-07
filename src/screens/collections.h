#pragma once
// Collections tab: the top-level collections; a collection shows its subcollections first,
// then its items, one page at a time.

#include <functional>
#include <vector>

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class CollectionsScreen final : public Screen {
public:
    explicit CollectionsScreen(std::function<void(const ItemRow&)> openItem, StatusFn status = {});
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const std::vector<CollectionEntry>& entries() const { return entries_; }  // this page
    std::vector<Rect> entryRects() const;
    static Rect searchRect();  // the header's search icon

private:
    void load(Library&);
    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    std::vector<CollectionEntry> all_, entries_;
    Pager pager_{0, 1};
};

class CollectionScreen final : public Screen {
public:
    struct Line {
        bool isCollection = false;
        CollectionEntry collection;
        ItemRow item;
    };

    CollectionScreen(CollectionEntry, std::function<void(const ItemRow&)> openItem, StatusFn status = {});
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const std::vector<Line>& page() const { return page_; }
    int64_t total() const { return pager_.total(); }
    static Rect backRect();

private:
    void load(Library&);
    std::vector<Rect> lineRects() const;

    CollectionEntry entry_;
    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    std::vector<CollectionEntry> subs_;
    std::vector<Line> page_;
    Pager pager_{0, 1};
};

}  // namespace zk
