#pragma once
// Settings → Plugins: the installed plugins, one row per folder (by name): its name and version, a
// second line (its problem, "Turned off — it stopped working", its badge, or what it is), and an
// on/off toggle. A plugin never turned on isn't turned on from here: its toggle opens its page,
// which shows what it can do first. "Turn all plugins off" at the top. A row opens its page.

#include <functional>
#include <string>
#include <vector>

#include "plugins/host.h"
#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

// The current plugin host, or null when there is none (stopped for an update's install).
using PluginHostFn = std::function<PluginHost*()>;

std::string pluginName(const PluginStatus&);  // the manifest's name (on one line), else the folder's
// The list's second line: "Can't load: …", "Incompatible: needs app 0.4.0", "Turned off — it
// stopped working", "New — not turned on yet", its last error, its badge, or its description.
std::string pluginNote(const PluginStatus&);
// The same, but an update the catalog has ("Update to 1.3") before the quiet notes. `catalog` may be null.
std::string pluginNote(const PluginStatus&, CatalogService* catalog);
// A problem folder, a plugin the app turned off, or one failing now.
bool pluginNeedsAttention(const PluginStatus&);
// The Settings row: "3 on · 1 needs attention", "None installed", "Not available" (no host).
std::string pluginsSummary(PluginHost*);

// Why plugins aren't running this time ("" when they are), and a call when the list is shown (the
// Home notice of an automatic safe mode goes once the user has seen it).
struct PluginsOff {
    std::string reason;
    std::function<void()> seen;
};

class PluginsScreen final : public Screen {
public:
    explicit PluginsScreen(PluginHostFn, PluginsOff off = {});
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    const std::vector<PluginStatus>& plugins() const { return plugins_; }  // every one, in list order
    std::vector<Rect> rowRects() const;                                    // this page's rows
    Rect toggleRect(int row) const;  // on a row of this page; empty for one that can't be turned on
    std::string note(int row) const;  // that row's second line
    Rect turnAllOffRect() const;      // empty when none is on
    int page() const { return pager_.page(); }
    // With the catalog (Nav::catalog): "Update all (n)" at the top when updates wait, and "Get
    // plugins" the last row (empty when not on this page).
    Rect updateAllRect() const;
    std::string updateAllLabel() const;
    Rect getPluginsRect() const;

private:
    void load();
    int waiting() const;
    int perPage() const;
    int top() const;  // the first row's y: below the "Plugins are off" line when there is one
    const PluginStatus* at(int row) const;

    PluginHostFn host_;
    PluginsOff off_;
    CatalogService* catalog_ = nullptr;
    int catalogSerial_ = -1;
    uint64_t hostVersion_ = 0;
    std::vector<PluginStatus> plugins_;
    Pager pager_{0, 1};
};

}  // namespace zk
