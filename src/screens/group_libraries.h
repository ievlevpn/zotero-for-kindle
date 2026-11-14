#pragma once
// Settings → Group libraries (spec: group libraries, "Which groups sync"): every group the Kindle
// knows, by name, with a switch each.
//
//   Lab group                                   ( on)
//   1,204 items · read-only
//   Old project
//   No longer available                 [ Remove from Kindle ]
//
// Turning one off stops its syncing and hides it at once; its copy stays on the Kindle until the
// user says yes to removing it (a sheet saying how many of its books are downloaded here). With
// changes not sent yet, the sheet says so and offers Sync first. A group that is gone (left,
// deleted, out of the key's reach) offers Remove from Kindle. Nothing is deleted on its own, and the
// deletion itself runs on the sync worker (SyncControl::removeLibrary). A key that can't read groups
// gets one quiet line about making a new key; a listed group the key can't read says so, with no
// switch (and Remove only if something of it is still here). A group whose last sync failed says
// "Can't sync: …".

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cairo.h>

#include "screens/screen.h"

namespace zk {

// What the Settings row says: "2 on · 1 off · 1 no longer available", "None yet", or that the key
// can't read groups.
std::string groupLibrariesSummary(const std::vector<LibraryInfo>& groups, GroupAccess);
// The quiet line for a key without group access.
constexpr const char* kNoGroupAccess =
    "Your API key can't read group libraries. Make a new key with group access at zotero.org/settings/keys.";

// The confirmation over the bottom of the screen: before turning off a group with unsent changes,
// and before removing a group's copy.
struct GroupSheet {
    enum class Kind { TurnOff, Remove };
    enum class Choice { None, SyncFirst, TurnOff, Remove, RemoveAnyway, Keep };
    Kind kind = Kind::Remove;
    LibraryInfo group;
    LibraryRemoval check;  // libraryRemovalCheck(): unsent changes, conflicts, files on the Kindle

    std::string title() const;
    std::string body() const;
    Choice primary() const;    // the filled button
    Choice secondary() const;  // the outlined one
    static std::string label(Choice);
};

class GroupLibrariesScreen final : public Screen {
public:
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }

    struct Row {
        LibraryInfo group;
        LibraryRemoval check;
        bool hasCopy = false;   // items or files of it on the Kindle
        bool removing = false;  // asked of the worker, not done yet
        bool syncing = false;   // Sync first: waiting for the sync before asking again
        std::string error;      // its last sync problem ("" none): "Can't sync: …"
    };
    const std::vector<Row>& rows() const { return rows_; }
    std::string note(const Row&) const;   // the row's second line
    Rect rowRect(size_t i) const;         // of this page's rows (index into rows())
    Rect toggleRect(size_t i) const;      // empty for a gone group
    Rect removeRect(size_t i) const;      // "Remove from Kindle"; empty when not offered
    bool removeOffered(const Row&) const;
    std::optional<Rect> accessLineRect() const;  // the key's quiet line, when shown
    const std::optional<GroupSheet>& sheet() const { return sheet_; }
    Rect sheetRect() const;
    Rect sheetButtonRect(int i) const;  // 0 primary, 1 secondary
    int page() const { return page_; }
    int pages() const { return static_cast<int>(starts_.size()); }

private:
    void load(Library&);
    void layout();
    int rowHeight(const Row&) const;
    void toggle(size_t i, Nav&);
    void askRemove(size_t i, Nav&);
    void choose(GroupSheet::Choice, Nav&);
    // Sync first's own sync finished: off (as asked); on success the copy's removal is offered, on
    // failure the user is told the group was turned off and its copy kept.
    void afterSync(const SyncEvent&, Nav&);
    Rect content() const;
    size_t indexOf(int64_t lib) const;

    std::vector<Row> rows_;
    GroupAccess access_ = GroupAccess::Unknown;
    std::vector<size_t> starts_;  // each page's first row
    int page_ = 0;
    std::optional<GroupSheet> sheet_;
    struct Waiting {
        int64_t lib = 0;
        GroupSheet::Kind kind = GroupSheet::Kind::TurnOff;
        bool reEnabled = false;  // switched on again to send its changes: off again afterwards
        uint64_t request = 0;    // the sync asked for (SyncControl::syncNowTracked)
    };
    std::optional<Waiting> waiting_;  // Sync first
};

}  // namespace zk
