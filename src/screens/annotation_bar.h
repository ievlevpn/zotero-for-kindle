#pragma once
// The bar over a selection or a tapped highlight (spec §6.3 state 3): Zotero's 8 colours as
// gray dots with their names, then Highlight · Underline · Note (new) or Comment · Delete
// (existing). Picking a colour only selects it; the buttons commit.

#include <array>
#include <string>

#include <cairo.h>

#include "core/geometry.h"

namespace zk {

struct ZoteroColour {
    const char* hex;
    const char* name;
};
const std::array<ZoteroColour, 8>& zoteroColours();

class AnnotationBar {
public:
    enum class Mode { Create, Edit };
    enum class Action { None, Colour, Highlight, Underline, Note, Comment, Delete };
    struct Hit {
        Action action = Action::None;
        int colour = -1;
    };

    // `anchor`: the selection or highlight; the bar sits above it, or below if there's no room.
    AnnotationBar(Mode, Rect anchor, std::string colour);

    Mode mode() const { return mode_; }
    Rect rect() const { return rect_; }
    Rect dotRect(int i) const;     // 0..7, Zotero's order
    Rect buttonRect(int i) const;  // Create: 0 Highlight, 1 Underline, 2 Note; Edit: 0 Comment, 1 Delete
    const std::string& colour() const { return colour_; }
    void setColour(std::string c) { colour_ = std::move(c); }

    void draw(cairo_t*) const;
    Hit hit(Point) const;

    static constexpr int kH = 250;

private:
    int buttons() const { return mode_ == Mode::Create ? 3 : 2; }

    Mode mode_;
    Rect rect_;
    std::string colour_;
};

}  // namespace zk
