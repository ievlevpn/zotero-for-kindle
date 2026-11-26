#include "plugins/ui_tree.h"

#include <algorithm>
#include <initializer_list>
#include <set>

#include "plugins/manifest.h"

namespace zk {

using json = nlohmann::json;

namespace {

struct Bad {
    std::string msg;
};

[[noreturn]] void bad(const std::string& where, const std::string& what) { throw Bad{where + ": " + what}; }

bool falsy(const json& j) {
    return j.is_null() || (j.is_boolean() && !j.get<bool>()) || (j.is_string() && j.get_ref<const std::string&>().empty()) ||
           (j.is_number() && j.get<double>() == 0);
}

// Text that may hold line breaks: each line cleaned like oneLine, a space kept at its ends (spans
// join: "Filed into " + "Probability").
std::string multiLine(const std::string& s) {
    std::string out;
    size_t start = 0;
    for (;;) {
        const size_t nl = s.find('\n', start);
        const std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string c = oneLine(line);
        const auto blank = [](char ch) { return static_cast<unsigned char>(ch) <= 0x20; };
        if (!line.empty() && blank(line.front()) && !c.empty()) c = " " + c;
        if (!line.empty() && blank(line.back())) c += ' ';
        if (c == "  ") c = " ";
        out += c;
        if (nl == std::string::npos) break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

std::string trimmed(const std::string& s) {
    const auto blank = [](char ch) { return static_cast<unsigned char>(ch) <= 0x20; };
    size_t a = 0, b = s.size();
    while (a < b && blank(s[a])) ++a;
    while (b > a && blank(s[b - 1])) --b;
    return s.substr(a, b - a);
}

struct Parser {
    TreeUse use = TreeUse::Screen;
    size_t blocks = 0, text = 0;
    std::set<std::string> ids;

    void count(const std::string& where, size_t n = 1) {
        blocks += n;
        if (blocks > kMaxTreeBlocks) bad(where, "the tree has more than 2,000 blocks");
    }

    const json* field(const json& o, const char* name) {
        auto it = o.find(name);
        if (it == o.end() || it->is_null()) return nullptr;
        return &*it;
    }

    std::string str(const json& o, const char* name, const std::string& where, bool required, bool lines = false) {
        const json* v = field(o, name);
        if (!v) {
            if (required) bad(where, std::string("\"") + name + "\" is missing");
            return {};
        }
        if (!v->is_string()) bad(where, std::string("\"") + name + "\" must be a string");
        const std::string& s = v->get_ref<const std::string&>();
        text += s.size();
        if (text > kMaxTreeText) bad(where, "the tree has more than 256 KB of text");
        return lines ? multiLine(s) : oneLine(s);
    }

    bool flag(const json& o, const char* name, const std::string& where) {
        const json* v = field(o, name);
        if (!v) return false;
        if (!v->is_boolean()) bad(where, std::string("\"") + name + "\" must be true or false");
        return v->get<bool>();
    }

    int count1(const json& o, const char* name, const std::string& where, int def, int max) {
        const json* v = field(o, name);
        if (!v) return def;
        if (!v->is_number_integer() || v->get<long long>() < 1) bad(where, std::string("\"") + name + "\" must be a whole number, 1 or more");
        return static_cast<int>(std::min<long long>(v->get<long long>(), max));
    }

    template <class E>
    E choice(const json& o, const char* name, const std::string& where, E def,
             std::initializer_list<std::pair<const char*, E>> options) {
        const json* v = field(o, name);
        if (!v) return def;
        std::string all;
        for (const auto& [n, e] : options) {
            if (v->is_string() && v->get_ref<const std::string&>() == n) return e;
            all += all.empty() ? std::string("\"") + n + "\"" : std::string(", \"") + n + "\"";
        }
        bad(where, std::string("\"") + name + "\" must be one of " + all);
    }

    template <class E>
    std::optional<E> optChoice(const json& o, const char* name, const std::string& where,
                               std::initializer_list<std::pair<const char*, E>> options) {
        if (!field(o, name)) return std::nullopt;
        return choice<E>(o, name, where, E{}, options);
    }

    // "<n>%" with n from lo to hi, as n; else bad.
    int percent(const json& v, const char* name, const std::string& where, int lo, int hi, const std::string& said = "") {
        const std::string msg = !said.empty() ? said
            : std::string("\"") + name + "\" must be a percentage from " + std::to_string(lo) + "% to " + std::to_string(hi) + "%";
        if (!v.is_string()) bad(where, msg);
        const std::string& s = v.get_ref<const std::string&>();
        if (s.size() < 2 || s.size() > 4 || s.back() != '%') bad(where, msg);
        int n = 0;
        for (size_t i = 0; i + 1 < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') bad(where, msg);
            n = n * 10 + (s[i] - '0');
        }
        if (n < lo || n > hi) bad(where, msg);
        return n;
    }

    void widthOf(const json& o, const std::string& where, Block& b) {
        const json* v = field(o, "width");
        if (!v) return;
        if (v->is_string() && *v == "fill") b.width = Block::Width::Fill;
        else if (v->is_string() && *v == "auto") b.width = Block::Width::Auto;
        else {
            b.width = Block::Width::Percent;
            b.widthPct = percent(*v, "width", where, 1, 100, "\"width\" must be \"fill\", \"auto\" or a percentage from 1% to 100%");
        }
    }

    // An item as a ref: its key, or { key, library?, label? }.
    ItemRef itemRef(const json& r, const std::string& at) {
        ItemRef ref;
        if (r.is_string()) {
            const std::string& s = r.get_ref<const std::string&>();
            text += s.size();
            if (text > kMaxTreeText) bad(at, "the tree has more than 256 KB of text");
            ref.key = oneLine(s);
        } else if (r.is_object()) {
            onlyFieldsPlain(r, at, {"key", "library", "label"});
            ref.key = str(r, "key", at, true);
            ref.label = str(r, "label", at, false);
            if (const json* lib = field(r, "library")) {
                if (!lib->is_number_integer() || lib->get<long long>() < 0) bad(at, "\"library\" must be a whole number, 0 or more");
                ref.library = lib->get<long long>();
            }
        } else {
            bad(at, "an item must be a key or { key, library?, label? }");
        }
        if (ref.key.empty()) bad(at, "an item needs a key");
        return ref;
    }

    // The one block a box or an item holds, in `children[0]`.
    Block onlyChild(const json& o, const char* name, const std::string& where, int depth) {
        const json* v = field(o, name);
        if (!v || falsy(*v)) bad(where, std::string("\"") + name + "\" is missing");
        return block(*v, where, true, depth + 1);
    }

    std::string id(const json& o, const std::string& where) {
        std::string s = str(o, "id", where, true);
        if (s.empty()) bad(where, "\"id\" must not be empty");
        if (!ids.insert(s).second) bad(where, "another block already has the id \"" + s + "\"");
        return s;
    }

    const json* widthOn = nullptr;  // the block being parsed: any block may carry a width (0.12)

    void onlyFields(const json& o, const std::string& where, std::initializer_list<const char*> names) {
        for (auto it = o.begin(); it != o.end(); ++it) {
            bool known = it.key() == "kind" || (&o == widthOn && it.key() == "width");
            for (const char* n : names) known = known || it.key() == n;
            if (!known) bad(where, "unknown field \"" + it.key() + "\"");
        }
    }

    Block::Gap gapOf(const json& o, const char* name, const std::string& where, Block::Gap def, bool none) {
        using G = Block::Gap;
        if (none) return choice<G>(o, name, where, def, {{"none", G::None}, {"xsmall", G::XSmall}, {"small", G::Small}, {"medium", G::Medium}, {"large", G::Large}});
        return choice<G>(o, name, where, def, {{"small", G::Small}, {"medium", G::Medium}, {"large", G::Large}});
    }

    // The children of a layout block: falsy entries skipped, paths keep the written index.
    std::vector<Block> children(const json& o, const char* name, const std::string& where, int depth) {
        const json* v = field(o, name);
        if (!v) bad(where, std::string("\"") + name + "\" is missing");
        if (!v->is_array()) bad(where, std::string("\"") + name + "\" must be a list");
        std::vector<Block> out;
        for (size_t i = 0; i < v->size(); ++i) {
            const json& c = (*v)[i];
            if (falsy(c)) continue;
            out.push_back(block(c, where + "[" + std::to_string(i) + "]", true, depth + 1));
        }
        return out;
    }

    Block button(const json& o, const std::string& where) {
        if (!o.is_object()) bad(where, "a button must be an object");
        onlyFields(o, where, {"id", "title", "primary"});
        if (o.contains("kind") && o["kind"] != "button") bad(where, "must be a button");
        count(where);
        Block b;
        b.kind = Block::Kind::Button;
        b.id = id(o, where);
        b.title = str(o, "title", where, true);
        b.primary = flag(o, "primary", where);
        return b;
    }

    Block card(const json& o, const std::string& where) {
        onlyFields(o, where, {"title", "meta", "body", "badge", "math"});
        Block b;
        b.kind = Block::Kind::Card;
        b.title = str(o, "title", where, true);
        b.meta = str(o, "meta", where, false);
        b.body = str(o, "body", where, false, true);
        b.badge = str(o, "badge", where, false);
        b.math = flag(o, "math", where);
        return b;
    }

    // `prefix` is the path up to this block ("column[2]"); its kind is appended (".buttons").
    Block block(const json& o, const std::string& prefix, bool child, int depth) {
        if (!o.is_object()) bad(prefix.empty() ? "the screen" : prefix, "a block must be an object");
        const json* k = field(o, "kind");
        if (!k || !k->is_string()) bad(prefix.empty() ? "the screen" : prefix, "a block needs a \"kind\"");
        const std::string kind = k->get<std::string>();
        const std::string where = prefix + (child ? "." : "") + kind;
        static const std::pair<const char*, Block::Kind> kinds[] = {
            {"column", Block::Kind::Column}, {"row", Block::Kind::Row},         {"text", Block::Kind::Text},
            {"card", Block::Kind::Card},     {"deck", Block::Kind::Deck},       {"items", Block::Kind::Items},
            {"list", Block::Kind::List},
            {"search", Block::Kind::Search}, {"field", Block::Kind::Field},     {"chips", Block::Kind::Chips},
            {"button", Block::Kind::Button}, {"buttons", Block::Kind::Buttons}, {"divider", Block::Kind::Divider},
            {"space", Block::Kind::Space},   {"notice", Block::Kind::Notice},   {"quote", Block::Kind::Quote},
            {"spacer", Block::Kind::Spacer}, {"box", Block::Kind::Box},         {"cover", Block::Kind::Cover},
            {"progress", Block::Kind::Progress}, {"item", Block::Kind::Item},   {"grid", Block::Kind::Grid},
            {"image", Block::Kind::Image},
        };
        const auto* found = std::find_if(std::begin(kinds), std::end(kinds), [&](const auto& p) { return kind == p.first; });
        if (found == std::end(kinds)) bad(prefix.empty() ? "the screen" : prefix, "unknown block kind \"" + kind + "\"");
        if (depth > kMaxTreeDepth) bad(where, "blocks nested more than 16 deep");
        if (use == TreeUse::HomeBody && (found->second == Block::Kind::Search || found->second == Block::Kind::Field))
            bad(prefix, kind + " can't be on Home");
        widthOn = &o;
        if (found->second == Block::Kind::Button) {
            Block btn = button(o, where);
            widthOf(o, where, btn);
            return btn;
        }
        count(where);
        Block b;
        b.kind = found->second;
        widthOf(o, where, b);
        using K = Block::Kind;
        switch (b.kind) {
            case K::Column:
            case K::Row:
                if (b.kind == K::Row) onlyFields(o, where, {"children", "gap", "align"});
                else onlyFields(o, where, {"children", "gap", "fit"});
                b.gap = gapOf(o, "gap", where, Block::Gap::Small, true);
                if (b.kind == K::Row)
                    b.align = choice<Block::Align>(o, "align", where, Block::Align::Center,
                                                   {{"top", Block::Align::Top}, {"center", Block::Align::Center},
                                                    {"bottom", Block::Align::Bottom}, {"stretch", Block::Align::Stretch}});
                else {
                    b.fit = choice<Block::Fit>(o, "fit", where, Block::Fit::Page, {{"page", Block::Fit::Page}, {"trim", Block::Fit::Trim}});
                    if (field(o, "fit") && !(use == TreeUse::HomeBody && depth == 1)) bad(where + ".fit", "only on a Home body's root");
                }
                b.children = children(o, "children", where, depth);
                if (b.kind == K::Row && b.children.size() > kMaxRowBlocks) bad(where, "a row holds at most 12 blocks");
                break;
            case K::Text: {
                onlyFields(o, where, {"text", "style", "lines", "font", "size", "weight", "tone", "italic", "caps", "align", "id", "math"});
                if (o.contains("id")) b.id = id(o, where);  // a link (0.12)
                b.font = optChoice<Block::Font>(o, "font", where, {{"serif", Block::Font::Serif}, {"sans", Block::Font::Sans}});
                b.size = optChoice<Block::Size>(o, "size", where,
                                                {{"xsmall", Block::Size::XSmall}, {"small", Block::Size::Small}, {"body", Block::Size::Body},
                                                 {"large", Block::Size::Large}, {"title", Block::Size::Title}});
                b.weight = optChoice<Block::Weight>(o, "weight", where,
                                                    {{"regular", Block::Weight::Regular}, {"medium", Block::Weight::Medium}, {"bold", Block::Weight::Bold}});
                b.tone = optChoice<Block::Tone>(o, "tone", where,
                                                {{"ink", Block::Tone::Ink}, {"secondary", Block::Tone::Secondary}, {"inverse", Block::Tone::Inverse}});
                b.textAlign = optChoice<Block::TextAlign>(o, "align", where,
                                                          {{"start", Block::TextAlign::Start}, {"center", Block::TextAlign::Center}, {"end", Block::TextAlign::End}});
                b.italic = flag(o, "italic", where);
                b.caps = flag(o, "caps", where);
                b.math = flag(o, "math", where);
                b.style = choice<Block::Style>(o, "style", where, Block::Style::Body,
                                               {{"title", Block::Style::Title}, {"body", Block::Style::Body},
                                                {"meta", Block::Style::Meta}, {"label", Block::Style::Label}});
                b.lines = count1(o, "lines", where, 0, 1000);
                const json* t = field(o, "text");
                if (!t) bad(where, "\"text\" is missing");
                if (t->is_string()) {
                    b.spans.push_back({str(o, "text", where, true, true)});
                } else if (t->is_array()) {
                    for (size_t i = 0; i < t->size(); ++i) {
                        const json& s = (*t)[i];
                        const std::string at = where + ".text[" + std::to_string(i) + "]";
                        Span span;
                        if (s.is_string()) {
                            span.text = str(*t, i, at, true);
                        } else if (s.is_object()) {
                            onlyFieldsPlain(s, at, {"text", "bold", "italic"});
                            span.text = str(s, "text", at, true, true);
                            span.bold = flag(s, "bold", at);
                            span.italic = flag(s, "italic", at);
                        } else {
                            bad(at, "a span must be a string or { text, bold?, italic? }");
                        }
                        if (!span.text.empty()) b.spans.push_back(std::move(span));
                    }
                } else {
                    bad(where, "\"text\" must be a string or a list of spans");
                }
                break;
            }
            case K::Card: {
                Block c = card(o, where);
                c.kind = K::Card;
                return c;
            }
            case K::Deck: {
                onlyFields(o, where, {"id", "card", "empty"});
                b.id = id(o, where);
                b.empty = str(o, "empty", where, false);
                auto c = o.find("card");
                if (c != o.end() && !falsy(*c)) {
                    if (!c->is_object()) bad(where + ".card", "must be a card");
                    if (c->contains("kind") && (*c)["kind"] != "card") bad(where + ".card", "must be a card");
                    count(where);
                    b.children.push_back(card(*c, where + ".card"));
                }
                break;
            }
            case K::Items: {
                onlyFields(o, where, {"id", "style", "items", "empty"});
                b.id = id(o, where);
                b.empty = str(o, "empty", where, false);
                b.itemStyle = choice<Block::ItemStyle>(o, "style", where, Block::ItemStyle::Row,
                                                       {{"card", Block::ItemStyle::Card}, {"row", Block::ItemStyle::Row},
                                                        {"shelf", Block::ItemStyle::Shelf}});
                if (!field(o, "style")) bad(where, "\"style\" is missing");
                const json* refs = field(o, "items");
                if (!refs) bad(where, "\"items\" is missing");
                if (!refs->is_array()) bad(where, "\"items\" must be a list");
                for (size_t i = 0; i < refs->size(); ++i) {
                    const std::string at = where + ".items[" + std::to_string(i) + "]";
                    ItemRef ref = itemRef((*refs)[i], at);
                    count(at);
                    b.items.push_back(std::move(ref));
                }
                break;
            }
            case K::List: {
                onlyFields(o, where, {"id", "rows", "empty"});
                b.id = id(o, where);
                b.empty = str(o, "empty", where, false);
                const json* rows = field(o, "rows");
                if (!rows) bad(where, "\"rows\" is missing");
                if (!rows->is_array()) bad(where, "\"rows\" must be a list");
                std::set<std::string> rowIds;
                for (size_t i = 0; i < rows->size(); ++i) {
                    const std::string at = where + ".rows[" + std::to_string(i) + "]";
                    const json& r = (*rows)[i];
                    if (!r.is_object()) bad(at, "a row must be an object");
                    onlyFieldsPlain(r, at, {"id", "title", "subtitle", "meta", "badge", "selected"});
                    count(at);
                    ListRow row;
                    row.id = str(r, "id", at, true);
                    if (row.id.empty()) bad(at, "\"id\" must not be empty");
                    if (!rowIds.insert(row.id).second) bad(at, "another row already has the id \"" + row.id + "\"");
                    row.title = str(r, "title", at, true);
                    row.subtitle = str(r, "subtitle", at, false);
                    row.meta = str(r, "meta", at, false);
                    row.badge = str(r, "badge", at, false);
                    row.selected = flag(r, "selected", at);
                    b.rows.push_back(std::move(row));
                }
                break;
            }
            case K::Search:
            case K::Field: {
                if (b.kind == K::Search) onlyFields(o, where, {"id", "placeholder", "value", "focus"});
                else onlyFields(o, where, {"id", "label", "placeholder", "value", "lines", "focus"});
                b.id = id(o, where);
                b.placeholder = str(o, "placeholder", where, false);
                if (field(o, "value")) b.value = str(o, "value", where, false, b.kind == K::Field);
                b.focus = flag(o, "focus", where);
                if (b.kind == K::Field) {
                    b.label = str(o, "label", where, false);
                    b.lines = count1(o, "lines", where, 1, 8);
                }
                break;
            }
            case K::Chips: {
                onlyFields(o, where, {"id", "chips", "selected", "select"});
                b.id = id(o, where);
                b.select = choice<Block::Select>(o, "select", where, Block::Select::Many,
                                                 {{"none", Block::Select::None}, {"one", Block::Select::One},
                                                  {"many", Block::Select::Many}});
                const json* cs = field(o, "chips");
                if (!cs) bad(where, "\"chips\" is missing");
                if (!cs->is_array()) bad(where, "\"chips\" must be a list");
                std::set<std::string> chipIds;
                for (size_t i = 0; i < cs->size(); ++i) {
                    const std::string at = where + ".chips[" + std::to_string(i) + "]";
                    const json& c = (*cs)[i];
                    Chip chip;
                    if (c.is_string()) {
                        chip.id = str(*cs, i, at);
                        chip.title = chip.id;
                    } else if (c.is_object()) {
                        onlyFieldsPlain(c, at, {"id", "title"});
                        chip.id = str(c, "id", at, true);
                        chip.title = str(c, "title", at, true);
                    } else {
                        bad(at, "a chip must be a string or { id, title }");
                    }
                    if (chip.id.empty()) bad(at, "a chip needs an id");
                    if (!chipIds.insert(chip.id).second) bad(at, "another chip already has the id \"" + chip.id + "\"");
                    count(at);
                    b.chips.push_back(std::move(chip));
                }
                if (const json* sel = field(o, "selected")) {
                    if (!sel->is_array()) bad(where, "\"selected\" must be a list of chip ids");
                    for (size_t i = 0; i < sel->size(); ++i) {
                        if (!(*sel)[i].is_string()) bad(where, "\"selected\" must be a list of chip ids");
                        b.selected.push_back(str(*sel, i, where));
                    }
                }
                break;
            }
            case K::Buttons: {
                onlyFields(o, where, {"buttons"});
                const json* bs = field(o, "buttons");
                if (!bs) bad(where, "\"buttons\" is missing");
                if (!bs->is_array()) bad(where, "\"buttons\" must be a list");
                for (size_t i = 0; i < bs->size(); ++i) {
                    if (falsy((*bs)[i])) continue;
                    if (b.children.size() == 3) bad(where, "at most 3");
                    b.children.push_back(button((*bs)[i], where + "[" + std::to_string(i) + "]"));
                }
                break;
            }
            case K::Divider: onlyFields(o, where, {}); break;
            case K::Space:
                onlyFields(o, where, {"size"});
                b.gap = gapOf(o, "size", where, Block::Gap::Medium, false);
                break;
            case K::Notice:
                onlyFields(o, where, {"text", "id"});
                b.text = str(o, "text", where, true);
                if (o.contains("id")) b.id = id(o, where);  // tappable (0.9)
                break;
            case K::Quote:  // a passage (0.11): its text as written, its source on one line; tappable with an id
                onlyFields(o, where, {"text", "source", "id"});
                b.text = trimmed(str(o, "text", where, true, true));  // a highlight's edges are noise here
                b.meta = str(o, "source", where, false);
                if (o.contains("id")) b.id = id(o, where);
                break;
            case K::Spacer: onlyFields(o, where, {}); break;
            case K::Box: {
                onlyFields(o, where, {"child", "id", "pad", "border", "fill", "radius"});
                if (o.contains("id")) b.id = id(o, where);  // tappable
                b.pad = gapOf(o, "pad", where, Block::Gap::None, true);
                b.border = choice<Block::Border>(o, "border", where, Block::Border::None,
                                                 {{"none", Block::Border::None}, {"thin", Block::Border::Thin}, {"thick", Block::Border::Thick}});
                b.fill = choice<Block::Fill>(o, "fill", where, Block::Fill::None,
                                             {{"none", Block::Fill::None}, {"light", Block::Fill::Light}, {"ink", Block::Fill::Ink}});
                b.radius = choice<Block::Radius>(o, "radius", where, Block::Radius::None,
                                                 {{"none", Block::Radius::None}, {"small", Block::Radius::Small}, {"round", Block::Radius::Round}});
                b.children.push_back(onlyChild(o, "child", where, depth));
                break;
            }
            case K::Cover: {
                onlyFields(o, where, {"item", "width"});
                const json* it = field(o, "item");
                if (!it) bad(where, "\"item\" is missing");
                b.items.push_back(itemRef(*it, where + ".item"));
                break;
            }
            case K::Progress: {
                onlyFields(o, where, {"value", "width"});
                const json* v = field(o, "value");
                if (!v) bad(where, "\"value\" is missing");
                if (!v->is_number() || !(v->get<double>() >= 0 && v->get<double>() <= 1)) bad(where, "\"value\" must be a number from 0 to 1");
                b.fraction = v->get<double>();
                break;
            }
            case K::Item: {
                onlyFields(o, where, {"item", "child"});
                const json* it = field(o, "item");
                if (!it) bad(where, "\"item\" is missing");
                b.items.push_back(itemRef(*it, where + ".item"));
                b.children.push_back(onlyChild(o, "child", where, depth));
                break;
            }
            case K::Grid: {
                onlyFields(o, where, {"children", "columns", "minWidth", "gap", "rows"});
                b.gap = gapOf(o, "gap", where, Block::Gap::Small, true);
                if (const json* c = field(o, "columns")) {
                    if (c->is_string() && *c == "auto") b.columns = 0;
                    else if (c->is_number_integer() && c->get<long long>() >= 1 && c->get<long long>() <= 12) b.columns = c->get<int>();
                    else bad(where, "\"columns\" must be \"auto\" or a whole number from 1 to 12");
                }
                if (const json* m = field(o, "minWidth")) b.minWidthPct = percent(*m, "minWidth", where, 5, 100);
                if (const json* r = field(o, "rows")) {
                    if (!r->is_number_integer() || r->get<long long>() < 1 || r->get<long long>() > 20)
                        bad(where, "\"rows\" must be a whole number from 1 to 20");
                    b.maxRows = r->get<int>();
                }
                b.children = children(o, "children", where, depth);
                break;
            }
            case K::Image: {
                onlyFields(o, where, {"src", "width", "id"});
                if (o.contains("id")) b.id = id(o, where);
                b.src = str(o, "src", where, true);
                const std::string& p = b.src;
                bool dotdot = false;
                for (size_t a = 0; a <= p.size();) {
                    size_t e = p.find_first_of("/\\", a);
                    if (e == std::string::npos) e = p.size();
                    if (p.compare(a, e - a, "..") == 0) dotdot = true;
                    a = e + 1;
                }
                if (p.empty()) bad(where, "\"src\" must not be empty");
                if (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':')) bad(where, "\"src\" must be a path inside the plugin, not an absolute one");
                if (dotdot) bad(where, "\"src\" must not contain \"..\"");
                break;
            }
            case K::Button: break;  // handled above
        }
        return b;
    }

    // An array element that must be a string (a span, a chip, a selected id).
    std::string str(const json& arr, size_t i, const std::string& where, bool lines = false) {
        const std::string& s = arr[i].get_ref<const std::string&>();
        text += s.size();
        if (text > kMaxTreeText) bad(where, "the tree has more than 256 KB of text");
        return lines ? multiLine(s) : oneLine(s);
    }

    void onlyFieldsPlain(const json& o, const std::string& where, std::initializer_list<const char*> names) {
        for (auto it = o.begin(); it != o.end(); ++it) {
            bool known = false;
            for (const char* n : names) known = known || it.key() == n;
            if (!known) bad(where, "unknown field \"" + it.key() + "\"");
        }
    }
};

}  // namespace

std::optional<Block> parseTree(const json& j, std::string* why, TreeUse use) {
    try {
        Parser p;
        p.use = use;
        return p.block(j, "", false, 1);
    } catch (const Bad& b) {
        if (why) *why = b.msg;
    } catch (const json::exception& e) {
        if (why) *why = std::string("the screen: ") + e.what();
    }
    return std::nullopt;
}

size_t blockCount(const Block& b) {
    // an items block counts its refs; a cover's or an item's one ref is part of the block
    size_t n = 1 + b.rows.size() + b.chips.size() + (b.kind == Block::Kind::Items ? b.items.size() : 0);
    for (const auto& c : b.children) n += blockCount(c);
    return n;
}

std::string plainText(const Block& b) {
    std::string s;
    for (const auto& sp : b.spans) s += sp.text;
    return s;
}

}  // namespace zk
