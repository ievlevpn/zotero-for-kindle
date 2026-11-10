#pragma once
// Get plugins (Settings → Plugins → the last row): the catalog's plugins by name, each with its
// author · description and "Installed", "Update" or nothing on the right; paged. "As of 9 Nov" and
// Check now at the top; with no copy yet and no network, "Connect to Wi-Fi to see the plugins in
// the catalog." Opening it checks when the copy is more than an hour old. A row opens the plugin's
// page: part 1's for an installed one, CatalogPageScreen for the others. Needs Nav::catalog().

#include <string>
#include <vector>

#include "catalog/registry.h"
#include "screens/plugins.h"
#include "ui/pager.h"

namespace zk {

// "As of 9 Nov" for a wall-clock time (local), "" for 0.
std::string asOf(int64_t wallMs);

class CatalogListScreen final : public Screen {
public:
    explicit CatalogListScreen(PluginHostFn);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    struct Row {
        std::string id, name, byline, mark;  // mark: "Installed", "Update" or ""
    };
    std::vector<Row> rows() const;  // this page's
    std::vector<Rect> rowRects() const;
    std::string headline() const;  // "As of 9 Nov", "Checking…", or the Wi-Fi message
    Rect checkRect() const;
    int pages() const { return pager_.pages(); }

private:
    void load();
    int perPage() const;

    PluginHostFn host_;
    CatalogService* catalog_ = nullptr;
    std::vector<Row> all_;
    Pager pager_{0, 1};
    int serial_ = -1;
    bool wantCheck_ = false;  // the check on opening, until the catalog was free to start it
};

}  // namespace zk
