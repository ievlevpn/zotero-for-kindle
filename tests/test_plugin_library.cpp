#include <doctest/doctest.h>

#include <filesystem>

#include "fake_plugins.h"
#include "plugins/host.h"
#include "plugins/identifiers.h"
#include "store/library.h"

using namespace zk;
using namespace zk::test;
using namespace zk::plugins;
using json = nlohmann::ordered_json;

namespace {

const PluginEvent kRun{"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

ItemRecord record(const std::string& key, const std::string& title, const nlohmann::json& data, const std::string& type = "journalArticle") {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = type;
    r.title = title;
    r.json = data.dump();
    r.dateAdded = "2024-01-01T00:00:00Z";
    return r;
}

// A plugin that runs `body` on syncFinished; `out(x)` logs, and an error is logged as ERR.
std::string pluginWith(const std::string& body) {
    return "const out = (x) => console.log('OUT ' + (typeof x === 'string' ? x : x === undefined ? 'undefined' : JSON.stringify(x)));\n"
           "zk.on('syncFinished', async () => { try {\n" + body +
           "\n} catch (e) { console.log('ERR ' + e.name + ' ' + e.message + (e.permission ? ' perm=' + e.permission : '')); } });\n";
}

struct Env {
    FakePlugins fp;
    std::string dbPath = fp.path("library.db");
    Db db = Db::open(dbPath);
    Library lib{db};

    PluginHostDeps deps() {
        PluginHostDeps d = fp.deps();
        d.dbPath = dbPath;
        d.engineEnv = {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache-plugin-lib"};
        return d;
    }
    std::string run(const std::string& id) {
        PluginHost host(deps());
        host.setEnabled(id, true);
        host.start();
        host.dispatch(kRun);
        REQUIRE(host.waitIdle(8000));
        return fp.log(id);
    }
};

void fill(Library& lib) {
    lib.upsertCollection({"COLLTOP1", "Reading", "", 1});
    lib.upsertCollection({"COLLSUB1", "arXiv", "COLLTOP1", 1});
    lib.upsertCollection({"COLLOTH1", "Archive", "", 1});
    ItemRecord a = record("AAAAAAA1", "Gaussian random fields", {{"DOI", "10.1214/AOP/1176990650"}, {"abstractNote", "Fields."}});
    a.creators = {{"Robert", "Adler", "", "author"}, {"", "", "Some Institute", "editor"}};
    a.date = "1981";
    a.year = "1981";
    a.publication = "Annals";
    a.tags = {"to-read", "probability"};
    a.collections = {"COLLSUB1"};
    lib.upsertItem(a);
    ItemRecord b = record("BBBBBBB1", "Attention is all you need",
                          {{"archiveID", "arXiv:2401.01234v1"}, {"extra", "Citation Key: vaswani2017\nfoo"}}, "preprint");
    b.tags = {"read"};
    lib.upsertItem(b);
    lib.upsertItem(record("CCCCCCC1", "A book", {{"ISBN", "978-0-19-852663-6"}, {"url", "https://example.org/b"}}, "book"));
    lib.upsertItem(record("DDDDDDD1", "No identifiers", {}));
    AttachmentRecord pdf;
    pdf.key = "PDFPDFP1";
    pdf.parentKey = "AAAAAAA1";
    pdf.contentType = "application/pdf";
    pdf.filename = "gaussian.pdf";
    pdf.md5 = "abc123";
    pdf.linkMode = "imported_file";
    pdf.localPath = "tests/data/pdf/sample.pdf";
    pdf.state = AttachmentRecord::State::Local;
    lib.upsertAttachment(pdf);
    AttachmentRecord remote = pdf;
    remote.key = "REMOTEP1";
    remote.filename = "";
    remote.state = AttachmentRecord::State::Remote;
    remote.localPath = "";
    lib.upsertAttachment(remote);
    AnnotationRecord n1;
    n1.key = "ANNOTAT1";
    n1.parentKey = "PDFPDFP1";
    n1.type = "highlight";
    n1.text = "random fields";
    n1.comment = "nice";
    n1.color = "#ffd400";
    n1.pageLabel = "12";
    n1.sortIndex = "00002|000100|00100";
    n1.tags = {"idea"};
    n1.dateModified = "2025-03-02T10:00:00Z";
    n1.json = R"({"dateAdded":"2025-03-01T10:00:00Z"})";
    lib.upsertAnnotation(n1);
    AnnotationRecord n2 = n1;
    n2.key = "ANNOTAT2";
    n2.type = "note";
    n2.text = "";
    n2.comment = "";
    n2.pageLabel = "";
    n2.tags = {};
    n2.sortIndex = "00001|000100|00100";
    n2.dateModified = "2025-04-02T10:00:00Z";
    n2.json = "{}";
    lib.upsertAnnotation(n2);
    NoteRecord note;
    note.key = "NOTENOTE";
    note.parentKey = "AAAAAAA1";
    note.html = "<p>hi</p>";
    note.dateModified = "2025-02-02T00:00:00Z";
    note.json = R"({"dateAdded":"2025-02-01T00:00:00Z"})";
    lib.upsertNote(note);
}

}  // namespace

// ---- identifiers ------------------------------------------------------------------------------

TEST_CASE("identifiers: DOIs are cleaned of prefixes and trailing punctuation") {
    CHECK(normaliseDoi("10.1000/ABC.def") == "10.1000/ABC.def");
    CHECK(normaliseDoi("https://doi.org/10.1000/abc") == "10.1000/abc");
    CHECK(normaliseDoi("http://dx.doi.org/10.1000/abc") == "10.1000/abc");
    CHECK(normaliseDoi("doi:10.1000/abc") == "10.1000/abc");
    CHECK(normaliseDoi("DOI: 10.1000/abc.") == "10.1000/abc");
    CHECK(normaliseDoi("  (10.1000/abc),;  ") == "10.1000/abc");
    CHECK(normaliseDoi("10.1002/(SICI)1097-4571(199806)49:8<693::AID-ASI4>3.0.CO;2-O") ==
          "10.1002/(SICI)1097-4571(199806)49:8<693::AID-ASI4>3.0.CO;2-O");
    CHECK(normaliseDoi("not a doi") == "");
    CHECK(normaliseDoi("") == "");
    CHECK(doiKey("https://doi.org/10.1000/AbC") == "10.1000/abc");
}

TEST_CASE("identifiers: arXiv ids drop prefixes, URLs and the version") {
    CHECK(normaliseArxiv("2401.01234") == "2401.01234");
    CHECK(normaliseArxiv("2401.01234v3") == "2401.01234");
    CHECK(normaliseArxiv("arXiv:2401.01234v3") == "2401.01234");
    CHECK(normaliseArxiv("arxiv: 2401.01234v3 [cs.LG]") == "2401.01234");
    CHECK(normaliseArxiv("https://arxiv.org/abs/2401.01234v3") == "2401.01234");
    CHECK(normaliseArxiv("http://www.arxiv.org/pdf/2401.01234v2.pdf") == "2401.01234");
    CHECK(normaliseArxiv("https://arxiv.org/abs/hep-th/9901001v2") == "hep-th/9901001");
    CHECK(normaliseArxiv("hep-th/9901001") == "hep-th/9901001");
    CHECK(normaliseArxiv("arXiv:math.GT/0309136v1") == "math.GT/0309136");
    CHECK(normaliseArxiv("0704.0001") == "0704.0001");
    CHECK(normaliseArxiv("2401.012345") == "");  // six digits is no arXiv id
    CHECK(normaliseArxiv("https://example.org/2401.01234") == "");
    CHECK(normaliseArxiv("hello") == "");
}

TEST_CASE("identifiers: ISBN-10 and ISBN-13 normalise to ISBN-13 digits") {
    CHECK(normaliseIsbn("978-0-19-852663-6") == "9780198526636");
    CHECK(normaliseIsbn("0-19-852663-6") == "9780198526636");
    CHECK(normaliseIsbn("0198526636") == "9780198526636");
    CHECK(normaliseIsbn("ISBN 080442957X") == "9780804429573");
    CHECK(normaliseIsbn("123") == "");
    CHECK(isbnKeys("0198526636, 978-1-4028-9462-6").size() == 2);
}

TEST_CASE("identifiers: doi, arXiv id and citation key come from fields, extra and URLs") {
    auto rec = [](const nlohmann::json& d) { return record("KKKKKKK1", "t", d); };
    CHECK(doiOf(rec({{"DOI", "https://doi.org/10.1/x"}})) == "10.1/x");
    CHECK(doiOf(rec({{"url", "https://doi.org/10.2/y"}})) == "10.2/y");
    CHECK(doiOf(rec({{"extra", "foo\nDOI: 10.3/z"}})) == "10.3/z");
    CHECK(doiOf(rec({{"url", "https://example.org/"}})) == "");
    CHECK(arxivIdOf(rec({{"archiveID", "arXiv:2401.01234v1"}})) == "2401.01234");
    CHECK(arxivIdOf(rec({{"extra", "arXiv:2401.01234 [cs.LG]"}})) == "2401.01234");
    CHECK(arxivIdOf(rec({{"url", "https://arxiv.org/abs/2401.01234v7"}})) == "2401.01234");
    CHECK(arxivIdOf(rec({{"DOI", "10.48550/arXiv.2401.01234"}})) == "2401.01234");
    CHECK(arxivIdOf(rec({{"title", "x"}})) == "");
    CHECK(citationKeyOf(rec({{"citationKey", "adler1981"}, {"extra", "Citation Key: other"}})) == "adler1981");
    CHECK(citationKeyOf(rec({{"extra", "x\nCitation Key: bbt2020\ny"}})) == "bbt2020");
    CHECK(citationKeyOf(rec({{"extra", "nothing"}})) == "");
    CHECK(citationKeyOf(rec({})) == "");
    ItemRecord broken = rec({});
    broken.json = "{not json";
    CHECK(doiOf(broken) == "");
}

TEST_CASE("identifiers: the Item shape") {
    ItemRecord r = record("AAAAAAA1", "T", {{"DOI", "10.1/x"}, {"abstractNote", "A"}, {"extra", "e"}, {"ISBN", "0198526636"}});
    r.creators = {{"Ann", "Lee", "", "author"}, {"", "", "Org", "editor"}};
    r.date = "March 2019";
    r.year = "2019";
    r.publication = "J";
    r.tags = {"a"};
    r.collections = {"C1"};
    json j = itemJson(r, true);
    CHECK(j["key"] == "AAAAAAA1");
    CHECK(j["type"] == "journalArticle");
    CHECK(j["title"] == "T");
    CHECK(j["creators"] == json::parse(R"([{"first":"Ann","last":"Lee","type":"author"},{"name":"Org","type":"editor"}])"));
    CHECK(j["year"] == 2019);
    CHECK(j["date"] == "March 2019");
    CHECK(j["publication"] == "J");
    CHECK(j["doi"] == "10.1/x");
    CHECK(j["arxiv"].is_null());
    CHECK(j["isbn"] == "0198526636");
    CHECK(j["url"].is_null());
    CHECK(j["abstract"] == "A");
    CHECK(j["tags"] == json::parse(R"(["a"])"));
    CHECK(j["collections"] == json::parse(R"(["C1"])"));
    CHECK(j["read"] == true);
    CHECK(j["extra"] == "e");
    CHECK(j["citationKey"].is_null());
    CHECK(j.size() == 17);
    ItemRecord empty = record("EEEEEEE1", "", {});
    empty.itemType = "book";
    json e = itemJson(empty, false);
    CHECK(e["year"].is_null());
    CHECK(e["date"].is_null());
    CHECK(e["publication"].is_null());
    CHECK(e["abstract"].is_null());
    CHECK(e["extra"].is_null());
    CHECK(e["creators"].is_array());
}

// ---- zk.library ---------------------------------------------------------------------------

TEST_CASE("library: items() iterates a collection lazily in pages") {
    Env env;
    env.lib.upsertCollection({"COLLBIG1", "Big", "", 1});
    {
        auto tx = env.lib.begin();
        for (int i = 0; i < 120; ++i) {
            char key[16];
            std::snprintf(key, sizeof key, "BIG%05d", i);
            ItemRecord r = record(key, "Item " + std::to_string(i), {});
            r.collections = {"COLLBIG1"};
            r.tags = {i % 2 ? "odd" : "even"};
            char added[32];
            std::snprintf(added, sizeof added, "2024-01-01T00:%02d:%02dZ", i / 60, i % 60);
            r.dateAdded = added;
            env.lib.upsertItem(r);
        }
        env.lib.upsertItem(record("OTHER001", "Elsewhere", {}));
        tx.commit();
    }
    env.fp.add("a", pluginWith(R"(
        let n = 0; const seen = new Set();
        for await (const item of zk.library.items({ collection: "COLLBIG1" })) { n++; seen.add(item.key); }
        out(["all", n, seen.size]);
        n = 0;
        for await (const item of zk.library.items({ collection: "COLLBIG1", limit: 7 })) n++;
        out(["limit", n]);
        let first;
        for await (const item of zk.library.items({ collection: "COLLBIG1" })) { first = item; break; }
        out(["first", first.key, first.title, first.read]);
        n = 0;
        for await (const item of zk.library.items({ tag: "odd" })) n++;
        out(["tag", n]);
        n = 0;
        for await (const item of zk.library.items()) n++;
        out(["everything", n]);
        n = 0;
        for await (const item of zk.library.items({ collection: "NOSUCHCO" })) n++;
        out(["none", n]);
        const names = [];
        try { zk.library.items({ limit: 0 }); } catch (e) { names.push(e.name); }
        try { zk.library.items({ collection: 5 }); } catch (e) { names.push(e.name); }
        out(names);
    )"), {"library:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT ["all",120,120])"));
    CHECK(contains(log, R"(OUT ["limit",7])"));
    CHECK(contains(log, R"(OUT ["first","BIG00119","Item 119",false])"));
    CHECK(contains(log, R"(OUT ["tag",60])"));
    CHECK(contains(log, R"(OUT ["everything",121])"));
    CHECK(contains(log, R"(OUT ["none",0])"));
    CHECK(contains(log, R"(OUT ["TypeError","TypeError"])"));
}

TEST_CASE("library: item(), search(), tags(), attachments()") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        const it = await zk.library.item("AAAAAAA1");
        out(it);
        out(await zk.library.item("ZZZZZZZZ"));
        const b = await zk.library.item("BBBBBBB1");
        out([b.read, b.citationKey, b.arxiv]);
        out((await zk.library.search("gaussian adler")).map((i) => i.key));
        out(await zk.library.search("   "));
        out(await zk.library.tags());
        out(await zk.library.attachments("AAAAAAA1"));
        out(await zk.library.attachments("DDDDDDD1"));
    )"), {"library:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT {"key":"AAAAAAA1","type":"journalArticle","title":"Gaussian random fields","creators":[{"first":"Robert","last":"Adler","type":"author"},{"name":"Some Institute","type":"editor"}],"year":1981,"date":"1981","publication":"Annals","doi":"10.1214/AOP/1176990650","arxiv":null,"isbn":null,"url":null,"abstract":"Fields.","tags":["to-read","probability"],"collections":["COLLSUB1"],"read":false,"extra":null,"citationKey":null})"));
    CHECK(contains(log, "OUT null"));
    CHECK(contains(log, R"(OUT [true,"vaswani2017","2401.01234"])"));
    CHECK(contains(log, R"(OUT ["AAAAAAA1"])"));
    CHECK(contains(log, "OUT []"));
    CHECK(contains(log, R"(OUT [{"name":"probability","count":1},{"name":"read","count":1},{"name":"to-read","count":1}])"));
    CHECK(contains(log, R"("key":"PDFPDFP1","itemKey":"AAAAAAA1","title":"gaussian.pdf","contentType":"application/pdf","filename":"gaussian.pdf","downloaded":true)"));
    CHECK(contains(log, R"("key":"REMOTEP1","itemKey":"AAAAAAA1","title":"","contentType":"application/pdf","filename":null,"downloaded":false)"));
}

TEST_CASE("library: findByIdentifier matches arXiv 2401.01234v3 to an item saved as 2401.01234v1") {
    Env env;
    fill(env.lib);
    env.lib.upsertItem(record("EEEEEEE1", "Old style", {{"url", "https://arxiv.org/abs/hep-th/9901001v1"}}, "preprint"));
    env.fp.add("a", pluginWith(R"(
        const keys = async (id) => (await zk.library.findByIdentifier(id)).map((i) => i.key);
        out(["v3", await keys({ arxiv: "2401.01234v3" })]);
        out(["bare", await keys({ arxiv: "2401.01234" })]);
        out(["prefix", await keys({ arxiv: "arXiv:2401.01234v2" })]);
        out(["abs", await keys({ arxiv: "https://arxiv.org/abs/2401.01234v9" })]);
        out(["pdf", await keys({ arxiv: "https://arxiv.org/pdf/2401.01234v9.pdf" })]);
        out(["old", await keys({ arxiv: "hep-th/9901001v4" })]);
        out(["miss", await keys({ arxiv: "2401.99999" })]);
        out(["isbn13", await keys({ isbn: "9780198526636" })]);
        out(["isbn10", await keys({ isbn: "0-19-852663-6" })]);
        out(["several", await keys({ arxiv: "2401.01234", isbn: "0198526636", doi: "10.1214/aop/1176990650" })]);
        out(["none", await keys({})]);
        out(["junk", await keys({ doi: "hello", arxiv: "x", isbn: "1" })]);
        const names = [];
        try { await zk.library.findByIdentifier({ doi: 5 }); } catch (e) { names.push(e.name); }
        try { await zk.library.findByIdentifier(); } catch (e) { names.push(e.name); }
        out(names);
    )"), {"library:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT ["v3",["BBBBBBB1"]])"));
    CHECK(contains(log, R"(OUT ["bare",["BBBBBBB1"]])"));
    CHECK(contains(log, R"(OUT ["prefix",["BBBBBBB1"]])"));
    CHECK(contains(log, R"(OUT ["abs",["BBBBBBB1"]])"));
    CHECK(contains(log, R"(OUT ["pdf",["BBBBBBB1"]])"));
    CHECK(contains(log, R"(OUT ["old",["EEEEEEE1"]])"));
    CHECK(contains(log, R"(OUT ["miss",[]])"));
    CHECK(contains(log, R"(OUT ["isbn13",["CCCCCCC1"]])"));
    CHECK(contains(log, R"(OUT ["isbn10",["CCCCCCC1"]])"));
    CHECK(contains(log, R"(OUT ["several",["AAAAAAA1","BBBBBBB1","CCCCCCC1"]])"));
    CHECK(contains(log, R"(OUT ["none",[]])"));
    CHECK(contains(log, R"(OUT ["junk",[]])"));
    CHECK(contains(log, R"(OUT ["TypeError","TypeError"])"));
}

TEST_CASE("library: findByIdentifier matches a DOI case-insensitively and with a https://doi.org/ prefix") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        const keys = async (id) => (await zk.library.findByIdentifier(id)).map((i) => i.key);
        out(["exact", await keys({ doi: "10.1214/AOP/1176990650" })]);
        out(["lower", await keys({ doi: "10.1214/aop/1176990650" })]);
        out(["url", await keys({ doi: "https://doi.org/10.1214/aop/1176990650" })]);
        out(["doi:", await keys({ doi: "doi:10.1214/aop/1176990650." })]);
        out(["miss", await keys({ doi: "10.1214/other" })]);
    )"), {"library:read"});
    const std::string log = env.run("a");
    for (const char* k : {"exact", "lower", "url", "doi:"}) CHECK(contains(log, std::string("OUT [\"") + k + R"(",["AAAAAAA1"]])"));
    CHECK(contains(log, R"(OUT ["miss",[]])"));
}

TEST_CASE("library: the identifier index follows changes made to the library meanwhile") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        out(["before", (await zk.library.findByIdentifier({ doi: "10.9/new" })).length]);
        for (let i = 0; i < 400 && !(await zk.library.item("NEWNEWN1")); i++) await new Promise((r) => setTimeout(r, 10));
        out(["after", (await zk.library.findByIdentifier({ doi: "10.9/new" })).map((i) => i.key)]);
    )"), {"library:read"});
    PluginHostDeps deps = env.deps();
    deps.handlerBudgetMs = 8000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(eventually([&] { return contains(env.fp.log("a"), "OUT [\"before\",0]"); }));
    env.lib.upsertItem(record("NEWNEWN1", "New", {{"DOI", "10.9/NEW"}}));  // this connection; the plugin has its own
    REQUIRE(host.waitIdle(8000));
    CHECK(contains(env.fp.log("a"), R"(OUT ["after",["NEWNEWN1"]])"));
}

TEST_CASE("library: collections() carry their path") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        out(await zk.library.collections());
    )"), {"library:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT [{"key":"COLLOTH1","name":"Archive","parent":null,"path":["Archive"]},{"key":"COLLTOP1","name":"Reading","parent":null,"path":["Reading"]},{"key":"COLLSUB1","name":"arXiv","parent":"COLLTOP1","path":["Reading","arXiv"]}])"));
}

TEST_CASE("library: feeds() lists the Zotero feed subscriptions by name") {
    Env env;
    env.lib.setMetaValue("zotero:feeds", R"({
        "https://rss.arxiv.org/rss/math.PR": ["math.PR updates on arXiv.org", 10, 30, 1440],
        "https://example.org/b": ["blog", 10, 30, 60],
        "https://example.org/odd": 7,
        "https://example.org/empty": []
    })");
    env.fp.add("a", pluginWith(R"(
        out(await zk.library.feeds());
    )"), {"library:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT [{"url":"https://example.org/b","name":"blog"},{"url":"https://example.org/empty","name":"https://example.org/empty"},{"url":"https://example.org/odd","name":"https://example.org/odd"},{"url":"https://rss.arxiv.org/rss/math.PR","name":"math.PR updates on arXiv.org"}])"));
}

TEST_CASE("library: feeds() is empty without the setting, or with a malformed one") {
    Env env;
    env.fp.add("a", pluginWith(R"(out(await zk.library.feeds());)"), {"library:read"});
    CHECK(contains(env.run("a"), "OUT []"));
    env.lib.setMetaValue("zotero:feeds", "[1, 2");
    CHECK(contains(env.run("a"), "OUT []"));
}

TEST_CASE("library: annotations, recentAnnotations and notes") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        const an = await zk.library.annotations("PDFPDFP1");
        out(an.map((a) => a.key));
        out(an[1]);
        out(an[0]);
        out((await zk.library.annotations("REMOTEP1")).length);
        const recent = [];
        for await (const a of zk.library.recentAnnotations()) recent.push(a.key);
        out(["recent", recent]);
        const since = [];
        for await (const a of zk.library.recentAnnotations({ since: "2025-04-01" })) since.push(a.key);
        out(["since", since]);
        const lim = [];
        for await (const a of zk.library.recentAnnotations({ limit: 1 })) lim.push(a.key);
        out(["limit", lim]);
        out(await zk.library.notes("AAAAAAA1"));
        out(await zk.library.notes("BBBBBBB1"));
    )"), {"annotations:read", "notes:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT ["ANNOTAT2","ANNOTAT1"])"));
    CHECK(contains(log, R"(OUT {"key":"ANNOTAT1","attachmentKey":"PDFPDFP1","itemKey":"AAAAAAA1","type":"highlight","text":"random fields","comment":"nice","color":"#ffd400","pageLabel":"12","tags":["idea"],"created":"2025-03-01T10:00:00Z","modified":"2025-03-02T10:00:00Z"})"));
    CHECK(contains(log, R"("key":"ANNOTAT2","attachmentKey":"PDFPDFP1","itemKey":"AAAAAAA1","type":"note","text":null,"comment":null)"));
    CHECK(contains(log, "OUT 0\n"));
    CHECK(contains(log, R"(OUT ["recent",["ANNOTAT2","ANNOTAT1"]])"));
    CHECK(contains(log, R"(OUT ["since",["ANNOTAT2"]])"));
    CHECK(contains(log, R"(OUT ["limit",["ANNOTAT2"]])"));
    CHECK(contains(log, R"(OUT [{"key":"NOTENOTE","itemKey":"AAAAAAA1","html":"<p>hi</p>","createdBy":null,"created":"2025-02-01T00:00:00Z","modified":"2025-02-02T00:00:00Z"}])"));
    CHECK(contains(log, "OUT []"));
}

TEST_CASE("library: annotations() without annotations:read is a PermissionError") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        out(!!(await zk.library.item("AAAAAAA1")));
        try { await zk.library.annotations("PDFPDFP1"); } catch (e) { out([e instanceof zk.PermissionError, e.message]); }
        try { for await (const a of zk.library.recentAnnotations()) out("no"); } catch (e) { out(e.permission); }
    )"), {"library:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT [true,"zk.library.annotations() needs \"annotations:read\" in plugin.json"])"));
    CHECK(contains(log, "OUT annotations:read"));
}

TEST_CASE("library: notes() needs notes:read") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        try { await zk.library.notes("AAAAAAA1"); out("no"); } catch (e) { out(e.name + ' ' + e.permission); }
    )"), {"library:read", "annotations:read"});
    CHECK(contains(env.run("a"), "OUT PermissionError notes:read"));
}

TEST_CASE("library: the rest needs library:read") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        const calls = {
          item: () => zk.library.item("AAAAAAA1"), search: () => zk.library.search("x"), tags: () => zk.library.tags(),
          collections: () => zk.library.collections(), attachments: () => zk.library.attachments("AAAAAAA1"),
          find: () => zk.library.findByIdentifier({ doi: "10.1/x" }), feeds: () => zk.library.feeds(),
          items: async () => { for await (const i of zk.library.items()) return; },
        };
        for (const [name, f] of Object.entries(calls)) {
          try { await f(); out(name + " allowed"); } catch (e) { out(name + " " + e.permission); }
        }
    )"), {"notes:read"});
    const std::string log = env.run("a");
    for (const char* n : {"item", "search", "tags", "collections", "attachments", "find", "items", "feeds"})
        CHECK(contains(log, std::string("OUT ") + n + " library:read"));
}

TEST_CASE("library: fullText needs fulltext:read") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        try { await zk.library.fullText("PDFPDFP1"); out("no"); } catch (e) { out(e.name + ' ' + e.permission); }
    )"), {"library:read"});
    CHECK(contains(env.run("a"), "OUT PermissionError fulltext:read"));
}

TEST_CASE("library: fullText returns a downloaded PDF's text and rejects what is not on the Kindle") {
    Env env;
    fill(env.lib);
    env.fp.add("a", pluginWith(R"(
        const text = await zk.library.fullText("PDFPDFP1");
        const pages = text.split("\f");
        out(["pages", pages.length >= 3, pages[1].includes("Hamiltonian"), pages[0].includes("Hamiltonian")]);
        try { await zk.library.fullText("REMOTEP1"); out("no"); } catch (e) { out(e.name + ': ' + e.message); }
        try { await zk.library.fullText("NOSUCHAT"); out("no"); } catch (e) { out(e.name + ': ' + e.message); }
        try { await zk.library.fullText(5); out("no"); } catch (e) { out(e.name); }
    )"), {"fulltext:read"});
    const std::string log = env.run("a");
    CHECK(contains(log, R"(OUT ["pages",true,true,false])"));
    CHECK(contains(log, "OUT Error: This file isn't on the Kindle"));
    CHECK(contains(log, "OUT Error: There is no attachment NOSUCHAT"));
    CHECK(contains(log, "OUT TypeError"));
}

TEST_CASE("library: without a library database the calls fail plainly") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(try { await zk.library.tags(); out("no"); } catch (e) { out(e.name + ': ' + e.message); })"), {"library:read"});
    PluginHost host(fp.deps());  // no dbPath
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    CHECK(contains(fp.log("a"), "OUT Error: zk.library: the library isn't available"));
}

TEST_CASE("library: findByIdentifier stays fast on a large library (the index is built once)") {
    Env env;
    {
        auto tx = env.lib.begin();
        for (int i = 0; i < 4000; ++i) {
            char key[16];
            std::snprintf(key, sizeof key, "LRG%05d", i);
            env.lib.upsertItem(record(key, "Item " + std::to_string(i),
                                      {{"DOI", "10.5555/x." + std::to_string(i)}, {"archiveID", "arXiv:2101." + std::to_string(10000 + i) + "v1"}}));
        }
        tx.commit();
    }
    env.fp.add("a", pluginWith(R"(
        const t0 = Date.now();
        let found = 0;
        for (let i = 0; i < 400; i += 1) {
          found += (await zk.library.findByIdentifier({ doi: "https://doi.org/10.5555/X." + (i * 10) })).length;
          found += (await zk.library.findByIdentifier({ arxiv: "2101." + (10000 + i * 10) + "v3" })).length;
        }
        out(["found", found, Date.now() - t0 < 4000]);
    )"), {"library:read"});
    PluginHostDeps deps = env.deps();
    deps.handlerBudgetMs = 20000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(25000));
    CHECK(contains(env.fp.log("a"), R"(OUT ["found",800,true])"));
}
