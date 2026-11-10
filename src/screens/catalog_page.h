#pragma once
// A catalog plugin that isn't installed (Get plugins → a row): its name, version (the latest
// tag), author, description and repo. Its permissions aren't known until the zip is downloaded, so
// Install ("Downloading…" while it works) downloads and verifies it, then this page gives way to
// part 1's page, which shows what it can do; the plugin stays off until turned on there. A
// failure says why in a plain sentence.

#include <string>

#include "screens/plugins.h"

namespace zk {

class CatalogPageScreen final : public Screen {
public:
    CatalogPageScreen(std::string id, PluginHostFn);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    std::string byline() const;      // "Version 1.3.0 · Ann Author"
    std::string installLabel() const;  // "Install" / "Downloading…"
    std::string failure() const;     // why the last install failed ("" if it didn't)
    Rect installRect() const;

private:
    struct Layout {
        Rect install;
    };
    Layout place(cairo_t*) const;

    std::string id_;
    PluginHostFn host_;
    CatalogService* catalog_ = nullptr;
    int serial_ = -1;
    bool asked_ = false;  // Install was tapped here
    bool lookedUp_ = false;  // its latest release was asked for
};

}  // namespace zk
