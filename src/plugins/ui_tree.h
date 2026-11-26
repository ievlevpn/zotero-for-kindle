#pragma once
// A plugin screen's tree of building blocks (zk.ui, sdk/types/zk.d.ts "Screens"): parsed from the
// JSON `render` returned, checked against the caps, its text cleaned. The parser accepts exactly
// what the zk.ui helpers produce (and the same objects written by hand); anything else is refused
// with a message naming where, e.g. `column[2].buttons: at most 3`.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zk {

constexpr size_t kMaxTreeBlocks = 2000;         // a list row, a chip or a button counts as one
constexpr size_t kMaxTreeText = 256 * 1024;     // bytes of text (every string in the tree)
constexpr int kMaxTreeDepth = 16;               // the root is at depth 1
constexpr size_t kMaxRowBlocks = 12;   // 0.12 (6 before)

struct Span {
    std::string text;
    bool bold = false, italic = false;
    bool operator==(const Span&) const = default;
};

struct ListRow {
    std::string id, title, subtitle, meta, badge;
    bool selected = false;
    bool operator==(const ListRow&) const = default;
};

struct Chip {
    std::string id, title;
    bool operator==(const Chip&) const = default;
};

// An item for an `items` block: its key, its library when the plugin says which, a card's kicker.
struct ItemRef {
    std::string key;
    std::optional<int64_t> library;
    std::string label;
    bool operator==(const ItemRef&) const = default;
};

struct Block {
    enum class Kind { Column, Row, Text, Card, Deck, Items, List, Search, Field, Chips, Button, Buttons, Divider, Space, Notice, Quote,
                      Spacer, Box, Cover, Progress, Item, Grid, Image };  // the last seven: 0.12
    enum class Gap { None, XSmall, Small, Medium, Large };  // also a space's size (XSmall, 6 px: 0.12, not a space's)
    enum class Style { Title, Body, Meta, Label };
    enum class Select { None, One, Many };
    enum class ItemStyle { Card, Row, Shelf };  // Shelf (0.11): cover tiles in rows; a ref's label isn't drawn
    // 0.12: the layout primitives. A text's options are unset (nullopt) unless given; `style` is the preset they override.
    enum class Width { Default, Fill, Auto, Percent };
    enum class Align { Top, Center, Bottom, Stretch };
    enum class Fit { Page, Trim };
    enum class Border { None, Thin, Thick };
    enum class Fill { None, Light, Ink };
    enum class Radius { None, Small, Round };
    enum class Font { Serif, Sans };
    enum class Size { XSmall, Small, Body, Large, Title };
    enum class Weight { Regular, Medium, Bold };
    enum class Tone { Ink, Secondary, Inverse, Auto };  // Auto is for the drawing; the parser never sets it
    enum class TextAlign { Start, Center, End };

    Kind kind = Kind::Column;
    std::string id;                // deck, list, search, field, chips, button; a notice's and a quote's (optional)
    std::vector<Block> children;   // column, row; a deck's card (0 or 1); the buttons of `buttons`
    Gap gap = Gap::Small;          // column, row; space (default Medium)
    std::vector<Span> spans;       // text ("\n" breaks a line)
    Style style = Style::Body;     // text
    int lines = 0;                 // text: at most (0: no cap); field: lines tall (1 by default)
    std::string title, meta, body, badge;  // card; a button's title; a quote's source (meta)
    std::string text;              // notice, quote (its line breaks kept)
    std::string empty;             // deck, list, items
    std::vector<ListRow> rows;     // list
    std::vector<ItemRef> items;    // items; cover, item: the one ref
    ItemStyle itemStyle = ItemStyle::Row;  // items; `empty` is shown when none can be read
    std::string label, placeholder;  // field, search
    std::optional<std::string> value;  // search, field
    bool focus = false, primary = false;
    std::vector<Chip> chips;       // chips
    std::vector<std::string> selected;
    Select select = Select::Many;

    Width width = Width::Default;  // any block; meaningful in a row (and a cover's, a progress's, an image's own)
    int widthPct = 0;              // width == Percent: 1..100
    Align align = Align::Center;   // row (centred, as rows were before 0.12, unless it says)
    Fit fit = Fit::Page;           // column, on a Home body's root only
    Gap pad = Gap::None;           // box (its child is children[0]); an item's child is children[0] too
    Border border = Border::None;  // box
    Fill fill = Fill::None;        // box
    Radius radius = Radius::None;  // box
    std::optional<Font> font;      // text (id: tappable, as a box's)
    std::optional<Size> size;
    std::optional<Weight> weight;
    std::optional<Tone> tone;
    std::optional<TextAlign> textAlign;
    bool italic = false, caps = false, math = false;
    double fraction = 0;           // progress: 0..1
    int columns = 0;               // grid: 0 = auto
    int minWidthPct = 25;          // grid: the narrowest a cell gets when columns is auto
    int maxRows = 0;               // grid: 0 = all (paged)
    std::string src;               // image: a path inside the plugin's package, never absolute or with ".."

    bool operator==(const Block&) const = default;
};

// Where a tree is shown: a screen of its own, or Home's body (no search or field: Home keeps its own).
enum class TreeUse { Screen, HomeBody };

// nullopt (and *why) when `j` is not a valid tree or goes over the caps.
std::optional<Block> parseTree(const nlohmann::json& j, std::string* why, TreeUse use = TreeUse::Screen);

// Blocks as the caps count them (list rows, chips, buttons and items one each).
size_t blockCount(const Block&);

// Deep equality, for diffing renders.
inline bool sameBlock(const Block& a, const Block& b) { return a == b; }

// The text of a text block, spans joined (for tests and plain uses).
std::string plainText(const Block&);

}  // namespace zk
