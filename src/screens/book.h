#pragma once
// A book or paper: cover, title, file, and its highlights. Plan 4 turns this into the reader;
// until then it is where an opened item lands.

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class BookScreen final : public Screen {
public:
    explicit BookScreen(ItemRow row);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const std::vector<AnnotationRecord>& highlights() const { return all_; }
    bool loading() const { return loading_; }

private:
    void load(Library&);
    Rect listArea() const;

    ItemRow row_;
    std::string fileLine_;
    std::vector<AnnotationRecord> all_;
    Pager pager_{0, 1};
    bool loading_ = false;
};

}  // namespace zk
