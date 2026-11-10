#pragma once
// A plugin screen's tree of building blocks (zk.ui, sdk/types/zk.d.ts "Screens"): parsed from the
// JSON `render` returned, checked against the caps, its text cleaned. The parser accepts exactly
// what the zk.ui helpers produce (and the same objects written by hand); anything else is refused
// with a message naming where, e.g. `column[2].buttons: at most 3`.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zk {

constexpr size_t kMaxTreeBlocks = 2000;         // a list row, a chip or a button counts as one
constexpr size_t kMaxTreeText = 256 * 1024;     // bytes of text (every string in the tree)

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

struct Block {
    enum class Kind { Column, Row, Text, Card, Deck, List, Search, Field, Chips, Button, Buttons, Divider, Space, Notice };
    enum class Gap { None, Small, Medium, Large };  // also a space's size
    enum class Style { Title, Body, Meta, Label };
    enum class Select { None, One, Many };

    Kind kind = Kind::Column;
    std::string id;                // deck, list, search, field, chips, button
    std::vector<Block> children;   // column, row; a deck's card (0 or 1); the buttons of `buttons`
    Gap gap = Gap::Small;          // column, row; space (default Medium)
    std::vector<Span> spans;       // text ("\n" breaks a line)
    Style style = Style::Body;     // text
    int lines = 0;                 // text: at most (0: no cap); field: lines tall (1 by default)
    std::string title, meta, body, badge;  // card; a button's title
    std::string text;              // notice
    std::string empty;             // deck, list
    std::vector<ListRow> rows;     // list
    std::string label, placeholder;  // field, search
    std::optional<std::string> value;  // search, field
    bool focus = false, primary = false;
    std::vector<Chip> chips;       // chips
    std::vector<std::string> selected;
    Select select = Select::Many;

    bool operator==(const Block&) const = default;
};

// nullopt (and *why) when `j` is not a valid tree or goes over the caps.
std::optional<Block> parseTree(const nlohmann::json& j, std::string* why);

// Blocks as the caps count them (list rows, chips and buttons one each).
size_t blockCount(const Block&);

// Deep equality, for diffing renders.
inline bool sameBlock(const Block& a, const Block& b) { return a == b; }

// The text of a text block, spans joined (for tests and plain uses).
std::string plainText(const Block&);

}  // namespace zk
