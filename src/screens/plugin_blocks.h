#pragma once
// A plugin screen's blocks (plugins/ui_tree.h) laid out, drawn in the app's style, and hit-tested.
//
// Paging, as the app's lists do: when the blocks don't fit, the ones that can page (lists, cards
// and decks, text, chips) share the room left by the others and turn page by page together, so
// buttons stay put while a long card body or list pages (mode "elastic"). If even that doesn't fit
// (a tall form on a small Kindle), the whole screen flows page after page instead. Either way the
// page indicator ("2 / 5") sits at the bottom and nothing is drawn outside the area.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "plugins/ui_tree.h"
#include "ui/text.h"

namespace zk {

// What the app adds to the tree when drawing: the keyboard's field, typed text, a tapped chip.
struct FocusState {
    std::string focused;                                     // the search/field with the keyboard
    std::map<std::string, std::string> texts;                // typed text by id (else the block's value)
    std::map<std::string, std::vector<std::string>> chips;   // a chips block's selection shown before the next render
};

struct Laid {
    struct Part {
        int index = 0;  // a list row, a chip, a button of `buttons`
        Rect rect;
        bool operator==(const Part&) const = default;
    };
    const Block* block = nullptr;
    Rect rect;
    std::vector<Laid> children;  // the root's: the top-level blocks shown on this page
    std::vector<Part> parts;
    int first = 0, count = 0;    // the units shown: text lines, list rows, chip lines, card body lines
    int pages = 1, page = 0;     // on the root: the screen's pages and the one laid out (clamped)
    std::shared_ptr<const KeptText> text;               // a text block's lines, laid out once
    std::vector<std::shared_ptr<const KeptText>> kept;  // on the root: every text block's, on any page
};

// Where a plugin screen's blocks go: under the header, inside the margins.
Rect blocksArea();

// Lays `root` out in `area` on `page` (clamped to the pages there are; see Laid::pages). The
// returned tree points into `root`, which must outlive it.
//
// A text block's text is laid out once and kept with the tree: drawing it again, turning a page or
// laying out a new tree in which the block's text, style and width are the same (while the earlier
// tree is still held) reuses that layout.
Laid layoutTree(const Block& root, Rect area, int page);

// How many times a text block's text has been laid out (for tests).
int64_t textBlockLayouts();

// The root's page indicator ("2 / 5"), empty when there is one page.
Rect pageIndicatorRect(const Laid& root);

void drawTree(cairo_t*, const Laid& root, const FocusState&);

struct Hit {
    std::string id;    // the block's
    std::string part;  // a list row's or a chip's id
    Block::Kind kind = Block::Kind::Button;
    Rect rect;         // what was hit (the row, the chip, the button)
};

// The interactive block under `p`: a button, a list row, a chip, a search/field, the deck.
std::optional<Hit> hitTest(const Laid& root, Point p);

// The laid block with this id (a search/field to redraw while typing), or nullptr.
const Laid* findLaid(const Laid& root, const std::string& id);

}  // namespace zk
