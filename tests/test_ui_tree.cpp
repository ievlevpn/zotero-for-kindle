// Plugin screen trees: parsing, validation and caps (src/plugins/ui_tree.h).

#include <doctest/doctest.h>

#include "plugins/ui_tree.h"

using namespace zk;
using json = nlohmann::json;
using K = Block::Kind;

namespace {

std::string whyNot(const json& j) {
    std::string why;
    auto t = parseTree(j, &why);
    CHECK_FALSE(t);
    return why;
}

Block ok(const json& j) {
    std::string why;
    auto t = parseTree(j, &why);
    INFO(why);
    REQUIRE(t);
    return *t;
}

}  // namespace

TEST_CASE("ui tree: every block kind parses as the zk.ui helpers write it") {
    const json j = json::parse(R"({"kind":"column","gap":"medium","children":[
      {"kind":"row","children":[{"kind":"text","text":"math.PR · 12 left","style":"label"},{"kind":"button","id":"feeds","title":"Feeds"}]},
      {"kind":"text","text":["Filed into ",{"text":"Probability / SPDEs","bold":true},{"text":" now","italic":true}],"style":"meta","lines":2},
      {"kind":"deck","id":"card","card":{"kind":"card","title":"Gaussian fields","meta":"Adler · 2024","body":"One.\n\nTwo.","badge":"In library"},"empty":"Nothing left"},
      {"kind":"card","title":"Alone"},
      {"kind":"list","id":"cols","rows":[{"id":"a","title":"A","subtitle":"s","meta":"3","badge":"already in","selected":true},{"id":"b","title":"B"}],"empty":"None"},
      {"kind":"search","id":"find","placeholder":"Collection","focus":true},
      {"kind":"field","id":"note","label":"Note","lines":3,"value":"x"},
      {"kind":"chips","id":"tags","chips":["to-read",{"id":"k1","title":"Probability"}],"selected":["to-read"],"select":"one"},
      {"kind":"buttons","buttons":[{"id":"discard","title":"Discard"},null,{"id":"keep","title":"Keep","primary":true}]},
      {"kind":"divider"},{"kind":"space","size":"large"},{"kind":"space"},
      {"kind":"notice","text":"Discarded."},
      null, false, "", 0
    ]})");
    Block t = ok(j);
    CHECK(t.kind == K::Column);
    CHECK(t.gap == Block::Gap::Medium);
    REQUIRE(t.children.size() == 13);
    CHECK(t.children[0].kind == K::Row);
    CHECK(t.children[0].children[1].id == "feeds");
    const Block& text = t.children[1];
    REQUIRE(text.spans.size() == 3);
    CHECK(text.spans[0].text == "Filed into ");
    CHECK(text.spans[1].bold);
    CHECK(text.spans[2].italic);
    CHECK(text.style == Block::Style::Meta);
    CHECK(text.lines == 2);
    CHECK(plainText(text) == "Filed into Probability / SPDEs now");
    const Block& deck = t.children[2];
    REQUIRE(deck.children.size() == 1);
    CHECK(deck.children[0].body == "One.\n\nTwo.");
    CHECK(deck.children[0].badge == "In library");
    CHECK(deck.empty == "Nothing left");
    CHECK(t.children[3].kind == K::Card);
    CHECK(t.children[4].rows.size() == 2);
    CHECK(t.children[4].rows[0].selected);
    CHECK(t.children[5].focus);
    CHECK_FALSE(t.children[5].value);
    CHECK(t.children[6].lines == 3);
    CHECK(t.children[6].value == std::optional<std::string>("x"));
    CHECK(t.children[7].chips[0].title == "to-read");
    CHECK(t.children[7].chips[1].title == "Probability");
    CHECK(t.children[7].select == Block::Select::One);
    CHECK(t.children[8].children.size() == 2);
    CHECK(t.children[8].children[1].primary);
    CHECK(t.children[10].gap == Block::Gap::Large);
    CHECK(t.children[11].gap == Block::Gap::Medium);
    CHECK(t.children[12].text == "Discarded.");
    // column, row, text, button, text, deck, card, card, list + 2 rows, search, field, chips + 2, buttons + 2,
    // divider, 2 spaces, notice
    CHECK(blockCount(t) == 23);
}

TEST_CASE("ui tree: a deck with no card, a lone block as the root, null optional fields") {
    Block d = ok(json::parse(R"({"kind":"deck","id":"d","card":null,"empty":"Nothing left to riffle"})"));
    CHECK(d.children.empty());
    ok(json::parse(R"({"kind":"deck","id":"d"})"));
    Block t = ok(json::parse(R"({"kind":"text","text":"Gone","style":null})"));
    CHECK(t.style == Block::Style::Body);
}

TEST_CASE("ui tree: unknown kinds and fields and wrong types are refused, naming the path") {
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"text","text":"a"},{"kind":"toggle","id":"t"}]})")) ==
          "column[1]: unknown block kind \"toggle\"");
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"row","children":[{"kind":"text","text":"a","colour":"red"}]}]})")) ==
          "column[0].row[0].text: unknown field \"colour\"");
    CHECK(whyNot(json::parse(R"({"kind":"numbered","id":"n"})")) == "the screen: unknown block kind \"numbered\"");
    CHECK(whyNot(json::parse(R"("hello")")) == "the screen: a block must be an object");
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"a","style":"huge"})")) ==
          "text: \"style\" must be one of \"title\", \"body\", \"meta\", \"label\"");
    CHECK(whyNot(json::parse(R"({"kind":"list","id":"l","rows":[{"id":"a","title":"A"},{"title":"B"}]})")) ==
          "list.rows[1]: \"id\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"button","title":"x"})")) == "button: \"id\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"a","lines":0})")) ==
          "text: \"lines\" must be a whole number, 1 or more");
    CHECK(whyNot(json::parse(R"({"kind":"column"})")) == "column: \"children\" is missing");
}

TEST_CASE("ui tree: at most 3 buttons in a row") {
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"text","text":"a"},{"kind":"space"},
        {"kind":"buttons","buttons":[{"id":"a","title":"A"},{"id":"b","title":"B"},{"id":"c","title":"C"},{"id":"d","title":"D"}]}]})")) ==
          "column[2].buttons: at most 3");
    // A skipped falsy entry doesn't count.
    ok(json::parse(R"({"kind":"buttons","buttons":[{"id":"a","title":"A"},false,{"id":"b","title":"B"},{"id":"c","title":"C"}]})"));
}

TEST_CASE("ui tree: interactive ids are unique in a screen; rows and chips within their block") {
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"button","id":"go","title":"Go"},{"kind":"search","id":"go"}]})")) ==
          "column[1].search: another block already has the id \"go\"");
    CHECK(whyNot(json::parse(R"({"kind":"list","id":"l","rows":[{"id":"a","title":"A"},{"id":"a","title":"B"}]})")) ==
          "list.rows[1]: another row already has the id \"a\"");
    CHECK(whyNot(json::parse(R"({"kind":"chips","id":"c","chips":["x","x"]})")) ==
          "chips.chips[1]: another chip already has the id \"x\"");
    // Row ids may repeat other blocks' ids (events name both).
    ok(json::parse(R"({"kind":"column","children":[{"kind":"button","id":"a","title":"A"},{"kind":"list","id":"l","rows":[{"id":"a","title":"A"}]}]})"));
}

TEST_CASE("ui tree: caps of 2,000 blocks and 256 KB of text") {
    json rows = json::array();
    for (int i = 0; i < 1999; ++i) rows.push_back({{"id", std::to_string(i)}, {"title", "r"}});
    ok(json{{"kind", "list"}, {"id", "l"}, {"rows", rows}});  // 1 + 1999
    rows.push_back({{"id", "x"}, {"title", "r"}});
    CHECK(whyNot(json{{"kind", "list"}, {"id", "l"}, {"rows", rows}}) == "list.rows[1999]: the tree has more than 2,000 blocks");

    json chips = json::array();
    for (int i = 0; i < 2000; ++i) chips.push_back("c" + std::to_string(i));
    CHECK(whyNot(json{{"kind", "chips"}, {"id", "c"}, {"chips", chips}}).find("more than 2,000 blocks") != std::string::npos);

    const std::string big(200 * 1024, 'a');
    ok(json{{"kind", "card"}, {"title", "t"}, {"body", big}});
    CHECK(whyNot(json{{"kind", "column"},
                      {"children", {{{"kind", "card"}, {"title", "t"}, {"body", big}}, {{"kind", "text"}, {"text", big}}}}}) ==
          "column[1].text: the tree has more than 256 KB of text");
}

TEST_CASE("ui tree: titles and labels on one line; text and bodies keep their line breaks") {
    Block t = ok(json::parse(R"({"kind":"column","children":[
      {"kind":"button","id":"b","title":"  File\ninto\t Probability​ "},
      {"kind":"card","title":"A\nB","meta":"x\r\ny","body":"Para one\nline two\n\nPara\u0007 two"},
      {"kind":"text","text":["Bold ",{"text":"word","bold":true}," end\nnext"]},
      {"kind":"list","id":"l","rows":[{"id":"r","title":"T\n2","subtitle":"s\u0000s"}]},
      {"kind":"notice","text":"a\n\nb"}
    ]})"));
    CHECK(t.children[0].title == "File into Probability");
    CHECK(t.children[1].title == "A B");
    CHECK(t.children[1].meta == "x y");
    CHECK(t.children[1].body == "Para one\nline two\n\nPara two");
    CHECK(plainText(t.children[2]) == "Bold word end\nnext");
    CHECK(t.children[3].rows[0].title == "T 2");
    CHECK(t.children[3].rows[0].subtitle == "s s");
    CHECK(t.children[4].text == "a b");
}

TEST_CASE("ui tree: sameBlock is deep equality") {
    const json j = json::parse(R"({"kind":"column","children":[{"kind":"list","id":"l","rows":[{"id":"a","title":"A"}]}]})");
    Block a = ok(j), b = ok(j);
    CHECK(sameBlock(a, b));
    b.children[0].rows[0].selected = true;
    CHECK_FALSE(sameBlock(a, b));
}

TEST_CASE("ui tree: blocks nest at most 16 deep; a 2,000-deep chain is refused at once") {
    const auto chain = [](int depth) {
        json j = {{"kind", "text"}, {"text", "leaf"}};
        for (int i = 1; i < depth; ++i) j = {{"kind", "column"}, {"children", json::array({j})}};
        return j;
    };
    Block t = ok(chain(16));
    CHECK(blockCount(t) == 16);
    const std::string why = whyNot(chain(17));
    CHECK(why.find("blocks nested more than 16 deep") != std::string::npos);
    CHECK(why.rfind("column[0].", 0) == 0);  // named by its path
    CHECK(whyNot(chain(2000)).find("nested more than 16 deep") != std::string::npos);
}

TEST_CASE("ui tree: a row holds at most 6 blocks") {
    json kids = json::array();
    for (int i = 0; i < 6; ++i) kids.push_back({{"kind", "text"}, {"text", "t"}});
    ok(json{{"kind", "row"}, {"children", kids}});
    kids.push_back({{"kind", "text"}, {"text", "t"}});
    CHECK(whyNot(json{{"kind", "row"}, {"children", kids}}) == "row: a row holds at most 6 blocks");
}
