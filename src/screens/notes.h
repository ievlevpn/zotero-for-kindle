#pragma once
// Notes tab: highlights and notes across the library, newest first, grouped by item. With
// annotations on demand it covers the books opened on the Kindle (and says so when empty).

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class NotesScreen final : public Screen {
public:
    struct Line {
        bool header = false;  // the item's title above its notes
        std::string itemKey, itemTitle;
        NoteRow note;
    };

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const std::vector<Line>& page() const { return lines_; }
    std::vector<Rect> lineRects() const;

private:
    void load(Library&);
    std::vector<Line> lines_;
    Pager pager_{0, 1};
    bool allOffline_ = false;
};

}  // namespace zk
