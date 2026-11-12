#pragma once
// Notes tab: highlights and notes across the library, newest first, grouped by item. With
// annotations on demand it covers the books opened on the Kindle (and says so when empty).
// Each note shows its sync state; a conflict has Re-apply and Discard (which asks first).
// The header's search icon opens the same list searching: a field in place of the title, the
// highlights and notes whose text, comment or tags match, by book and in book order.
// The header's Tag (0.7) filters the list by a tag of the annotations (the Library's tag picker);
// a line under the header then reads "tag: method · 12 notes", with Clear.

#include <functional>
#include <optional>

#include "screens/library_sheets.h"
#include "screens/screen.h"
#include "ui/pager.h"
#include "ui/text_field.h"

namespace zk {

class NotesScreen final : public Screen {
public:
    // Opens a book, at a highlight when given (the open flow: downloads first if needed).
    using OpenBook = std::function<void(const ItemRow&, std::optional<AnnotationRecord> at)>;
    static constexpr int64_t kPauseMs = 700;  // typing: the results follow after this pause (or Return)

    explicit NotesScreen(OpenBook open = {}, bool searching = false) : open_(std::move(open)), searching_(searching) {}

    struct Line {
        bool header = false;  // the item's title above its notes
        std::string itemKey, itemTitle;
        NoteRow note;
    };

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onTick(Nav&) override;
    bool wantsKeyboard() const override { return searching_; }
    bool showsTabBar() const override { return !searching_; }
    static Rect searchRect();  // the tab's search icon
    static Rect tagButtonRect();  // the tab's Tag (filter)
    const std::string& tag() const { return tag_; }
    void setTag(const std::string& tag, Nav&);  // "" none
    std::string filterLine() const;  // "tag: method · 12 notes"
    static Rect filterLineRect();    // under the header, while a tag filters the list
    static Rect clearRect();
    bool searching() const { return searching_; }
    const std::string& query() const { return query_; }

    const std::vector<Line>& page() const { return lines_; }
    std::vector<Rect> lineRects() const;
    bool confirmingDiscard() const { return !discardKey_.empty(); }
    static Rect confirmRect(int i);  // 0 Discard, 1 Cancel
    const Pager& pager() const { return pager_; }
    const JumpSheet* jumpSheet() const { return jump_ ? &*jump_ : nullptr; }
    Rect footerRect() const;  // the page pill (not while searching): First page and Go to page
    Rect sheetArea() const;

private:
    void load(Library&);
    Rect contentArea() const;
    int perPage() const;
    void search(Nav&);
    std::vector<Line> lines_;
    std::optional<JumpSheet> jump_;
    Pager pager_{0, 1};
    bool allOffline_ = false;
    std::string discardKey_;
    OpenBook open_;  // asking "Discard your Kindle change?" for this conflict
    bool searching_ = false;
    TextField field_;
    std::string query_;  // what the list shows matches for
    std::string tag_;    // the Tag filter ("" none)
    bool pending_ = false;
    int64_t lastKeyMs_ = 0;
    int bottom_ = 0;  // the list's bottom: above the tab bar, or the keyboard while searching
};

}  // namespace zk
