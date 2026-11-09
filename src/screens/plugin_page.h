#pragma once
// One plugin (Settings → Plugins → a row): its name, version, author and description; "What it can
// do" (its permissions as the spec words them, one per line); its badge and last errors; Turn on /
// Turn off; and its repairs: What it changed, Reset its data, Discard unsent changes (when it has
// some), Remove. Reset, Discard and Remove ask first, in the app's own confirmation box.
// A folder that can't load shows its problem and can't be turned on; it can still be removed.
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
    bool showsTabBar() const override { return false; }

    std::vector<std::string> permissionLines() const;
    std::vector<std::string> errorLines() const;  // up to 3, newest first
    std::string turnLabel() const;                // "Turn on" / "Turn off"; "" when it can't be
    Rect turnRect() const;                        // empty when it can't be turned on
    Rect changesRect() const;                     // What it changed (empty without a manifest)
    Rect resetRect() const;                       // Reset its data (empty without a manifest)
    Rect discardRect() const;                     // Discard unsent changes (empty when none)
    std::string discardLabel() const;             // "Discard 2 unsent changes"
    Rect removeRect() const;
    // The question being asked ("" when none), and its buttons.
    std::string question() const;
    Rect yesRect() const;
    Rect noRect() const;

private:
    enum class Ask { None, Reset, Discard, Remove };
    struct Layout {
        Rect turn, changes, reset, discard, remove;
    };
    void load(Nav&);
    Layout layout() const { return place(nullptr); }
    Layout place(cairo_t*) const;  // lays the page out; draws it too when given a cairo
    std::optional<PluginConfirm> confirm() const;
    std::string name() const;

    std::string id_;
    PluginHostFn host_;
    std::optional<PluginStatus> status_;  // nullopt: no longer installed (or no host)
    int pending_ = 0;
    Ask ask_ = Ask::None;
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
    int pages() const { return pager_.pages(); }

private:
    int perPage() const;

    std::string id_, name_;
    std::vector<Line> all_;
    Pager pager_{0, 1};
};

}  // namespace zk
