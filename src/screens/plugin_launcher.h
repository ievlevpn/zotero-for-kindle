#pragma once
// Plugins (the puzzle-piece icon on Home): one row per plugin screen marked `main` — its title,
// the plugin's name, its badge on the right. A row opens the screen. Managing plugins stays in
// Settings → Plugins.

#include <vector>

#include "screens/plugin_entries.h"
#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class PluginLauncherScreen final : public Screen {
public:
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    const std::vector<PluginLaunch>& rows() const { return rows_; }
    std::vector<Rect> rowRects() const;  // this page's rows

private:
    void load(Nav&);
    int perPage() const;

    std::vector<PluginLaunch> rows_;
    uint64_t version_ = 0;
    Pager pager_{0, 1};
};

}  // namespace zk
