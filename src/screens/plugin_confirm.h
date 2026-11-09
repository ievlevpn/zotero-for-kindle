#pragma once
// The app's own confirmation of a plugin's removal (zk.zotero.trash and friends): a box over
// whatever is on screen, drawn by App, which gives it every gesture until it is answered. The
// plugin can't draw, dismiss or skip it: only its two buttons answer (a tap elsewhere does
// nothing).
//
//   Feed Riffle wants to move 'Gaussian random fields (v1)' to the Zotero trash.
//   [ Move to trash ]  [ Cancel ]
//
// `lines`: the question first, then what goes when several removals are asked together (as many
// as fit, two lines each at most, then "and N more").

#include <optional>
#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"

namespace zk {

class PluginConfirm {
public:
    PluginConfirm(std::string pluginName, std::vector<std::string> lines, std::string action /* "Move to trash" … */);

    void draw(cairo_t*) const;
    // true: the action; false: Cancel; nullopt: not on a button (nothing happens).
    std::optional<bool> onTap(Point) const;
    Rect rect() const;  // the whole box (for damage)
    Rect yesRect() const;
    Rect noRect() const;

    const std::string& pluginName() const { return pluginName_; }
    const std::vector<std::string>& lines() const { return lines_; }
    const std::string& action() const { return action_; }

private:
    struct Layout {
        Rect box;
        int questionH = 0;
        int listed = 0;  // list items shown, up to 2 lines each (the rest: "and N more")
        int listH = 0;
        bool more = false;
    };
    Layout layout() const;
    int itemH(int i) const;  // list item i (lines_[i + 1]), with the space after it

    std::string pluginName_;
    std::vector<std::string> lines_;
    std::string action_;
};

}  // namespace zk
