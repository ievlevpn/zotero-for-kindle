#pragma once
// Search for books: one field (the Kindle's own keyboard), results as item rows above the keyboard.
// It matches titles, people, publications and tags; highlights are searched from the Notes tab.
// Keys echo instantly in the field; results update on Return or after a 700 ms pause, since
// every list update is an e-ink refresh. Opened from a collection, it searches that collection
// (and its subcollections) until its "In: <name> ✕" chip is tapped. With nothing searched yet,
// the recent searches (Return, or a result opened) are listed to run again.

#include <functional>
#include <string>
#include <vector>

#include "screens/item_list.h"
#include "screens/screen.h"
#include "ui/text_field.h"

namespace zk {

class SearchScreen final : public Screen {
public:
    static constexpr int64_t kPauseMs = 700;

    // scopeKey/scopeName: a collection to search in ("": the whole library).
    explicit SearchScreen(std::function<void(const ItemRow&)> openItem, StatusFn status = {}, std::string scopeKey = {},
                          std::string scopeName = {});
    void onSyncEvent(const SyncEvent&, Nav&) override;

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool wantsKeyboard() const override { return true; }
    bool showsTabBar() const override { return false; }

    static Rect fieldRect();
    Rect resultsArea(Nav&) const;
    Rect scopeRect() const;  // the "In: <collection> ✕" chip; empty when searching the whole library
    const std::string& scopeKey() const { return scopeKey_; }
    // The recent searches as shown (nothing searched yet), their rows and the Clear link.
    const std::vector<std::string>& recent() const { return recent_; }
    std::vector<Rect> recentRects() const;
    Rect clearRect() const;
    const ItemList& results() const { return list_; }
    std::string emptyMessage() const;

private:
    void search(Nav&);
    void leave(Nav&);
    int top() const;  // below the header and the scope chip
    void layout(Nav&);

    std::function<void(const ItemRow&)> openItem_;
    TextField field_;
    ItemList list_;
    std::string searched_;
    bool pending_ = false;
    int64_t lastKeyMs_ = 0;
    Rect areaCache_{};
    std::string scopeKey_, scopeName_;
    std::vector<std::string> recent_;
};

}  // namespace zk
