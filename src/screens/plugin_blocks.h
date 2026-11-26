#pragma once
// A plugin screen's blocks (plugins/ui_tree.h) laid out, drawn in the app's style, and hit-tested.
//
// Paging, as the app's lists do: when the blocks don't fit, the ones that can page (lists, cards
// and decks, text, chips) share the room left by the others and turn page by page together, so
// buttons stay put while a long card body or list pages (mode "elastic"). If even that doesn't fit
// (a tall form on a small Kindle), the whole screen flows page after page instead. Either way the
// page indicator ("2 / 5") sits at the bottom and nothing is drawn outside the area.
//
// A screen whose column holds a deck is laid out around it (0.9): the deck takes all the height the
// other blocks leave, the blocks after it sit at the bottom, and every card — short, long, or none —
// has that same size. A long card pages inside itself ("2 / 3" quietly in its bottom corner); the
// other blocks stay where they are on every page. (A screen too full to leave the deck kMinDeckH
// pages as above.)

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "plugins/ui_tree.h"
#include "store/library.h"
#include "ui/math.h"
#include "ui/text.h"

namespace zk {

// What the app adds to the tree when drawing: the keyboard's field, typed text, a tapped chip.
struct FocusState {
    std::string focused;                                     // the search/field with the keyboard
    std::map<std::string, std::string> texts;                // typed text by id (else the block's value)
    std::map<std::string, std::vector<std::string>> chips;   // a chips block's selection shown before the next render
};

// Where an `items` block gets its Zotero items, and what the app adds to how it draws them
// (screens/item_source.h has the one that reads the library).
class ItemSource {
public:
    virtual ~ItemSource() = default;
    // The row to draw for a ref, nullopt (skipped silently) when it is gone, unreadable or in a switched-off group.
    virtual std::optional<ItemRow> row(const ItemRef&) = 0;
    // A card's meta line ("13% · 3 highlights · opened today").
    virtual std::string cardMeta(const ItemRow&) = 0;
    // A row's download progress or error ("" when there is none).
    virtual std::string status(const std::string& attachmentKey) = 0;
};

// An image block's picture (0.12): its PNG in greys, loaded once while any laid tree holds it.
struct PluginImage;

struct Laid {
    struct Part {
        int index = 0;  // a list row, a chip, a button of `buttons`, an item (an item piece's: 0)
        Rect rect;
        std::optional<ItemRow> item;  // an items block's row or card

        bool operator==(const Part&) const = default;
    };
    const Block* block = nullptr;
    Rect rect;
    std::vector<Laid> children;  // the root's: the top-level blocks shown on this page
    std::vector<Part> parts;
    std::vector<ItemRow> items;  // an items block: the rows its refs resolved to, in order (skipped ones left out); a cover's, an item piece's: its row
    std::vector<std::string> itemLabels;  // and each one's label
    std::vector<std::string> itemMetas;   // and, for cards, the meta line (as of the layout; "" for rows)
    int first = 0, count = 0;    // the units shown: text lines, list rows, chip lines, card body lines
    int pages = 1, page = 0;     // on the root: the screen's pages and the one laid out (clamped)
    Rect corner;                 // on the root: a filling deck's page indicator, in the card's corner (else empty)
    std::shared_ptr<const KeptText> text;               // a text block's lines, laid out once
    std::shared_ptr<const KeptMathText> mathText;       // a math-enabled text block's lines, laid out once
    std::shared_ptr<const KeptMathText> cardTitleMath;  // a math-enabled card's title
    std::shared_ptr<const KeptMathText> cardBodyMath;   // a math-enabled card's body
    std::shared_ptr<const PluginImage> image;           // an image block's picture (none: it takes no room)
    std::vector<std::shared_ptr<const KeptText>> kept;  // on the root: every text block's, on any page
    std::vector<std::shared_ptr<const KeptMathText>> keptMath;  // on the root: every math block's, on any page
};

// How many tiles a shelf (an items block in the "shelf" style) puts in a row `width` wide: tiles of
// about 240 px (200 at least), 36 px apart, spanning the width (PW5 4, KT4 4, Scribe 6), at least 1.
int shelfColumns(int width);

// Where a plugin screen's blocks go: under the header, inside the margins.
Rect blocksArea();

// Lays `root` out in `area` on `page` (clamped to the pages there are; see Laid::pages). The
// returned tree points into `root`, which must outlive it.
//
// A text block's text is laid out once and kept with the tree: drawing it again, turning a page or
// laying out a new tree in which the block's text, style and width are the same (while the earlier
// tree is still held) reuses that layout.
//
// `items` resolves the refs of items blocks, covers and item pieces (once per layout); without it
// items blocks take no room and draw nothing, covers are blank and item pieces inert.
//
// `package` is the plugin's folder: an image's `src` is a path in it. Its PNG (at most 2 MB, 4 megapixels
// and 4096 px a side, read from its header before decoding) is loaded once while any laid tree holds it, in greys; a missing or unreadable one (or no package)
// takes no room, and is logged once.
//
// 0.12's pieces: a cover is 2:3 at its width (in a stretched row, at the top of its slot); a progress bar 6 px tall; a cover's, a progress bar's
// or an image's own `width` is a percent of what its parent gives it (in a row, the row sizes it).
// An item piece is its child, hit as the item (Hit::kind Item); while its file downloads, the
// status is drawn in a bar over the piece's bottom. A grid's cells are equal, in rows as tall as
// their tallest cell; it pages by row, or, with `rows`, shows at most that many and never pages.
//
// A root column with fit "trim" (a Home body's, 0.12) never pages: its children at their natural
// heights while they fit; the first that doesn't is trimmed if it can be (a grid to its rows that
// fit, a column to its children that fit, recursively) and dropped otherwise, and everything after
// it is dropped. A column cut to only its first child of several (a heading alone) is dropped. Laid::children holds only what's shown, so hit-testing and diffs see only that.
Laid layoutTree(const Block& root, Rect area, int page, ItemSource* items = nullptr, const std::string& package = {});

// How many times a text block's text has been laid out (for tests).
int64_t textBlockLayouts();

// The root's page indicator ("2 / 5"), empty when there is one page.
Rect pageIndicatorRect(const Laid& root);

void drawTree(cairo_t*, const Laid& root, const FocusState&, ItemSource* items = nullptr);

struct Hit {
    std::string id;    // the block's
    std::string part;  // a list row's or a chip's id
    Block::Kind kind = Block::Kind::Button;
    Rect rect;         // what was hit (the row, the chip, the button, the item)
    std::optional<ItemRow> item;  // an items block's or an item piece's: part is its key
};

// The interactive block under `p`: a button, a list row, an item, a chip, a search/field, the deck, a notice,
// a quote, a text, a box or an image with an id (the innermost one), an item piece whose ref resolved.
std::optional<Hit> hitTest(const Laid& root, Point p);

// The laid block with this id (a search/field to redraw while typing), or nullptr.
const Laid* findLaid(const Laid& root, const std::string& id);

}  // namespace zk
