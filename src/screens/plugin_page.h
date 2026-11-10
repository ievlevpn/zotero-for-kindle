#pragma once
// One plugin (Settings → Plugins → a row): its name, version, author and description; "What it can
// do" (its permissions as the spec words them, one per line; on a crowded page the rest behind
// "+N more", a page of their own); its badge and last errors; Turn on /
// Turn off; and its repairs: What it changed, Reset its data, Discard unsent changes (when it has
// some), Remove. Reset, Discard and Remove ask first, in the app's own confirmation box.
// A folder that can't load shows its problem and can't be turned on; it can still be removed.
//
// With the catalog (Nav::catalog): "From the catalog" / "Installed by hand" under the byline;
// "Update to 1.3" beside Turn on/off when the catalog has a newer version (applied at once when it
// asks for nothing more; else "This version also wants to:" lists what it adds, with Update and
// Not now, and the installed version keeps running until the yes); "Roll back to 1.2" among the
// repairs when a previous version is kept; Remove asks "Remove <Name> and the data it saved?" and
// deletes its folder, kept version, storage and state on the catalog's worker.
//
// PluginChangesScreen (What it changed): its change log, newest first, a line per change — what,
// on which item, and how it ended (Sent, Pending, Refused and why, Cancelled, Declined).

#include <optional>
#include <string>
#include <vector>

#include "screens/plugin_confirm.h"
#include "screens/plugins.h"
#include "store/plugin_changes.h"

namespace zk {

class PluginPageScreen final : public Screen {
public:
    PluginPageScreen(std::string id, PluginHostFn);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    std::vector<std::string> permissionLines() const;
    // The catalog's parts (empty / "" without one).
    std::string provenance() const;   // "From the catalog" / "Installed by hand"
    std::string updateLabel() const;  // "Update to 1.3.0", "Downloading…"; "" when there is none
    Rect updateRect() const;
    std::string rollBackLabel() const;  // "Roll back to 1.2.0"; "" when none is kept
    Rect rollBackRect() const;
    std::vector<std::string> errorLines() const;  // up to 3, newest first
    std::string turnLabel() const;                // "Turn on" / "Turn off"; "" when it can't be
    Rect turnRect() const;                        // empty when it can't be turned on; else always on the page
    // What it can do: how many lines show here; "+N more" (empty when all show) opens them all.
    int shownPermissions() const;
    Rect moreRect() const;
    Rect changesRect() const;                     // What it changed (empty without a manifest)
    Rect resetRect() const;                       // Reset its data (empty without a manifest)
    Rect discardRect() const;                     // Discard unsent changes (empty when none)
    std::string discardLabel() const;             // "Discard 2 unsent changes"
    Rect removeRect() const;
    // The question being asked ("" when none), and its buttons.
    std::string question() const;
    std::vector<std::string> confirmLines() const;  // the box's question and list (the re-ask's too)
    Rect yesRect() const;
    Rect noRect() const;

private:
    enum class Ask { None, Reset, Discard, Remove };
    struct Layout {
        Rect turn, update, more, changes, reset, discard, rollBack, remove;
        int permissions = 0;  // what it can do, shown here (the rest behind "+N more")
    };
    void load(Nav&);
    Layout layout() const { return place(nullptr); }
    Layout place(cairo_t*) const;  // lays the page out; draws it too when given a cairo
    std::optional<PluginConfirm> confirm() const;
    std::string name() const;

    bool reasking() const;  // the catalog staged an update of this one that adds permissions

    std::string id_;
    PluginHostFn host_;
    CatalogService* catalog_ = nullptr;
    int catalogSerial_ = -1;
    uint64_t hostVersion_ = 0;
    std::optional<PluginStatus> status_;  // nullopt: no longer installed (or no host)
    int pending_ = 0;
    Ask ask_ = Ask::None;
};

// What a manifest can do, as the page words it: `only` (null: all of its permissions), and with
// `old` given, only the hosts it doesn't have.
std::vector<std::string> permissionLinesOf(const PluginManifest&, const std::vector<Permission>* only = nullptr,
                                           const PluginManifest* old = nullptr);

// What it can do, every line, paged (from "+N more" on a crowded plugin page).
class PluginPermissionsScreen final : public Screen {
public:
    PluginPermissionsScreen(std::string pluginName, std::vector<std::string> lines);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }

    std::vector<std::string> lines() const;  // this page's
    int pages() const { return pager_.pages(); }

private:
    int perPage() const;

    std::string name_;
    std::vector<std::string> all_;
    Pager pager_{0, 1};
};

class PluginChangesScreen final : public Screen {
public:
    PluginChangesScreen(std::string pluginId, std::string pluginName);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }

    struct Line {
        std::string what, title, outcome;  // "Add the tag “to-read”", the item's title, "Sent"
    };
    std::vector<Line> lines() const;  // this page's
    int pages() const { return static_cast<int>(starts_.size()); }

private:
    std::string id_, name_;
    std::vector<Line> all_;
    std::vector<size_t> starts_{0};  // each page's first line (a refused change takes more room)
    size_t page_ = 0;
};

}  // namespace zk
