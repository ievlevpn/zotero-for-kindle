#pragma once
// Notes tab: highlights and notes across the library, newest first, grouped by item. With
// annotations on demand it covers the books opened on the Kindle (and says so when empty).
// Each note shows its sync state; a conflict has Re-apply and Discard (which asks first).

#include <functional>
#include <optional>

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class NotesScreen final : public Screen {
public:
    // Opens a book, at a highlight when given (the open flow: downloads first if needed).
    using OpenBook = std::function<void(const ItemRow&, std::optional<AnnotationRecord> at)>;
    explicit NotesScreen(OpenBook open = {}) : open_(std::move(open)) {}

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
    bool confirmingDiscard() const { return !discardKey_.empty(); }
    static Rect confirmRect(int i);  // 0 Discard, 1 Cancel

private:
    void load(Library&);
    std::vector<Line> lines_;
    Pager pager_{0, 1};
    bool allOffline_ = false;
    std::string discardKey_;
    OpenBook open_;  // asking "Discard your Kindle change?" for this conflict
};

}  // namespace zk
