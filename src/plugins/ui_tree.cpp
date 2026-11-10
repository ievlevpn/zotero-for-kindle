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

struct Parser {
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

    std::string id(const json& o, const std::string& where) {
        std::string s = str(o, "id", where, true);
        if (s.empty()) bad(where, "\"id\" must not be empty");
        if (!ids.insert(s).second) bad(where, "another block already has the id \"" + s + "\"");
        return s;
    }

    void onlyFields(const json& o, const std::string& where, std::initializer_list<const char*> names) {
        for (auto it = o.begin(); it != o.end(); ++it) {
            bool known = it.key() == "kind";
            for (const char* n : names) known = known || it.key() == n;
            if (!known) bad(where, "unknown field \"" + it.key() + "\"");
        }
    }

    Block::Gap gapOf(const json& o, const char* name, const std::string& where, Block::Gap def, bool none) {
        using G = Block::Gap;
        if (none) return choice<G>(o, name, where, def, {{"none", G::None}, {"small", G::Small}, {"medium", G::Medium}, {"large", G::Large}});
        return choice<G>(o, name, where, def, {{"small", G::Small}, {"medium", G::Medium}, {"large", G::Large}});
    }

    // The children of a layout block: falsy entries skipped, paths keep the written index.
    std::vector<Block> children(const json& o, const char* name, const std::string& where) {
        const json* v = field(o, name);
        if (!v) bad(where, std::string("\"") + name + "\" is missing");
        if (!v->is_array()) bad(where, std::string("\"") + name + "\" must be a list");
        std::vector<Block> out;
        for (size_t i = 0; i < v->size(); ++i) {
            const json& c = (*v)[i];
            if (falsy(c)) continue;
            out.push_back(block(c, where + "[" + std::to_string(i) + "]", true));
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
        onlyFields(o, where, {"title", "meta", "body", "badge"});
        Block b;
        b.kind = Block::Kind::Card;
        b.title = str(o, "title", where, true);
        b.meta = str(o, "meta", where, false);
        b.body = str(o, "body", where, false, true);
        b.badge = str(o, "badge", where, false);
        return b;
    }

    // `prefix` is the path up to this block ("column[2]"); its kind is appended (".buttons").
    Block block(const json& o, const std::string& prefix, bool child) {
        if (!o.is_object()) bad(prefix.empty() ? "the screen" : prefix, "a block must be an object");
        const json* k = field(o, "kind");
        if (!k || !k->is_string()) bad(prefix.empty() ? "the screen" : prefix, "a block needs a \"kind\"");
        const std::string kind = k->get<std::string>();
        const std::string where = prefix + (child ? "." : "") + kind;
        static const std::pair<const char*, Block::Kind> kinds[] = {
            {"column", Block::Kind::Column}, {"row", Block::Kind::Row},         {"text", Block::Kind::Text},
            {"card", Block::Kind::Card},     {"deck", Block::Kind::Deck},       {"list", Block::Kind::List},
            {"search", Block::Kind::Search}, {"field", Block::Kind::Field},     {"chips", Block::Kind::Chips},
            {"button", Block::Kind::Button}, {"buttons", Block::Kind::Buttons}, {"divider", Block::Kind::Divider},
            {"space", Block::Kind::Space},   {"notice", Block::Kind::Notice},
        };
        const auto* found = std::find_if(std::begin(kinds), std::end(kinds), [&](const auto& p) { return kind == p.first; });
        if (found == std::end(kinds)) bad(prefix.empty() ? "the screen" : prefix, "unknown block kind \"" + kind + "\"");
        if (found->second == Block::Kind::Button) return button(o, where);
        count(where);
        Block b;
        b.kind = found->second;
        using K = Block::Kind;
        switch (b.kind) {
            case K::Column:
            case K::Row:
                onlyFields(o, where, {"children", "gap"});
                b.gap = gapOf(o, "gap", where, Block::Gap::Small, true);
                b.children = children(o, "children", where);
                break;
            case K::Text: {
                onlyFields(o, where, {"text", "style", "lines"});
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
                onlyFields(o, where, {"text"});
                b.text = str(o, "text", where, true);
                break;
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

std::optional<Block> parseTree(const json& j, std::string* why) {
    try {
        Parser p;
        return p.block(j, "", false);
    } catch (const Bad& b) {
        if (why) *why = b.msg;
    } catch (const json::exception& e) {
        if (why) *why = std::string("the screen: ") + e.what();
    }
    return std::nullopt;
}

size_t blockCount(const Block& b) {
    size_t n = 1 + b.rows.size() + b.chips.size();
    for (const auto& c : b.children) n += blockCount(c);
    return n;
}

std::string plainText(const Block& b) {
    std::string s;
    for (const auto& sp : b.spans) s += sp.text;
    return s;
}

}  // namespace zk
