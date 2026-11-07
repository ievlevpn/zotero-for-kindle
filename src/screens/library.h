#pragma once
// Library tab: every item, sortable (Recent · Title · Author · Year) and filterable (All, EPUB,
// PDF, Unread, and the three most used tags), one page at a time.

#include <functional>

#include "screens/item_list.h"
#include "screens/screen.h"

namespace zk {

class LibraryScreen final : public Screen {
public:
    explicit LibraryScreen(std::function<void(const ItemRow&)> openItem);

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    void selectSort(SortBy, Nav&);
    void selectChip(int index, Nav&);
    std::vector<std::string> chipLabels() const;
    ItemList& list() { return list_; }

    static Rect listArea();
    static Rect chipsArea();
    static Rect sortButtonRect();

private:
    void apply(Nav&);
    Rect menuRect() const;

    std::function<void(const ItemRow&)> openItem_;
    ItemList list_;
    ListQuery query_;
    std::vector<std::pair<std::string, int64_t>> tags_;
    int chip_ = 0;
    bool menuOpen_ = false;
};

const char* sortLabel(SortBy);

}  // namespace zk
