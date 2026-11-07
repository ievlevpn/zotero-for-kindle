#pragma once
// Search: one field (the Kindle's own keyboard), results as item rows above the keyboard.
// Keys echo instantly in the field; results update on Return or after a 700 ms pause, since
// every list update is an e-ink refresh.

#include <functional>
#include <string>

#include "screens/item_list.h"
#include "screens/screen.h"
#include "ui/text_field.h"

namespace zk {

class SearchScreen final : public Screen {
public:
    static constexpr int64_t kPauseMs = 700;

    explicit SearchScreen(std::function<void(const ItemRow&)> openItem, StatusFn status = {});
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
    const ItemList& results() const { return list_; }
    std::string emptyMessage() const;
    std::string note(Nav&) const;  // "" when every highlight is searchable

private:
    void search(Nav&);
    void leave(Nav&);

    std::function<void(const ItemRow&)> openItem_;
    TextField field_;
    ItemList list_;
    std::string searched_;
    bool pending_ = false;
    int64_t lastKeyMs_ = 0;
    std::string noteCache_;
    Rect areaCache_{};
};

}  // namespace zk
