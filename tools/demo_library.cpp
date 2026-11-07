// zk-demo-library: a small library of public-domain books (the test corpus) for the README's
// screenshots; no real Zotero data. Highlights are ranges Zotero's own reader made
// (tests/data/cfi). Run from the repository root:
//   build-desktop/zk-demo-library local/demo/demo.sqlite
//   build-desktop/zotero --screenshot home OUT.png --db local/demo/demo.sqlite

#include "store/db.h"
#include "store/library.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace zk;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Book {
    const char *key, *title, *first, *last, *year, *type, *file;  // file: tests/data/epub/<file>.epub
    std::vector<const char*> collections, tags;
};

const std::vector<Book> kBooks = {
    {"PRIDE001", "Pride and Prejudice", "Jane", "Austen", "1813", "book", "pride-and-prejudice", {"COLNOVEL"}, {"to-read"}},
    {"MOBY0001", "Moby-Dick; or, The Whale", "Herman", "Melville", "1851", "book", "moby-dick-se", {"COLNOVEL"}, {}},
    {"ORIGIN01", "On the Origin of Species", "Charles", "Darwin", "1859", "book", "origin-of-species-se", {"COLSCIEN"},
     {"evolution"}},
    {"ALICE001", "Alice’s Adventures in Wonderland", "Lewis", "Carroll", "1865", "book", "alice-gutenberg-epub3",
     {"COLNOVEL"}, {}},
    {"FRANK001", "Frankenstein; or, The Modern Prometheus", "Mary", "Shelley", "1818", "book",
     "frankenstein-epub2-ncx", {"COLNOVEL"}, {"to-read"}},
    {"PRINCE01", "The Prince", "Niccolò", "Machiavelli", "1532", "book", "the-prince-notes", {"COLPOLIT"}, {}},
    {"WASTE001", "The Waste Land", "T. S.", "Eliot", "1922", "book", "waste-land-poetry", {"COLPOETR"}, {}},
    {"GIBBON01", "The History of the Decline and Fall of the Roman Empire", "Edward", "Gibbon", "1776", "book",
     "gibbon-footnotes", {"COLHISTO"}, {}},
    // Papers: metadata only (not downloaded), as most of a library is.
    {"TURING01", "On Computable Numbers, with an Application to the Entscheidungsproblem", "Alan", "Turing", "1936",
     "journalArticle", nullptr, {"COLPAPER"}, {"logic"}},
    {"SHANNON1", "A Mathematical Theory of Communication", "Claude", "Shannon", "1948", "journalArticle", nullptr,
     {"COLPAPER"}, {}},
    {"COASE001", "The Nature of the Firm", "Ronald", "Coase", "1937", "journalArticle", nullptr, {"COLPAPER"},
     {"economics"}},
    {"NASH0001", "Equilibrium Points in n-Person Games", "John", "Nash", "1950", "journalArticle", nullptr,
     {"COLPAPER"}, {"economics"}},
};

// Highlights from Zotero's reader (tests/data/cfi), picked by sort index: whole sentences.
const std::map<std::string, std::vector<std::string>> kHighlights = {
    {"pride-and-prejudice", {"00004|00004860", "00005|00001430", "00006|00000125"}},
    {"moby-dick-se", {"00004|00000367", "00007|00000170", "00008|00013915"}},
    {"origin-of-species-se", {"00003|00000248", "00004|00007363"}},
    {"frankenstein-epub2-ncx", {"00008|00000105", "00026|00006837"}},
    {"gibbon-footnotes", {"00084|00000064"}},
};

void addHighlights(Library& lib, const std::string& att, const std::string& file, int64_t& clock) {
    auto picks = kHighlights.find(file);
    if (picks == kHighlights.end()) return;
    std::ifstream in("tests/data/cfi/" + file + ".zotero.json");
    const json records = json::parse(in);
    static const char* colors[] = {"#ffd400", "#5fb236", "#2ea8e5"};
    int made = 0;
    for (const auto& r : records) {
        const std::string sortIndex = r["sortIndex"];
        if (std::find(picks->second.begin(), picks->second.end(), sortIndex) == picks->second.end()) continue;
        AnnotationRecord a;
        a.key = att.substr(0, 4) + "HL" + std::to_string(10 + made);
        a.parentKey = att;
        a.type = "highlight";
        a.text = r["text"];
        a.color = colors[made % 3];
        a.sortIndex = sortIndex;
        a.position = json{{"type", "FragmentSelector"}, {"value", r["cfi"]}}.dump();
        if (file == "pride-and-prejudice" && made == 0) a.comment = "Darcy at the first ball: pride, or shyness?";
        char mod[32];
        std::snprintf(mod, sizeof mod, "2026-11-%02dT%02d:00:00Z", 1 + (int)(clock % 6), (int)(8 + clock % 12));
        ++clock;
        a.dateModified = mod;
        a.version = 1;
        lib.upsertAnnotation(a);
        ++made;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: zk-demo-library OUT.sqlite (run from the repository root)\n");
        return 2;
    }
    const fs::path out = fs::absolute(argv[1]);
    fs::create_directories(out.parent_path() / "files");
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(out.string() + ext);
    Db db = Db::open(out.string());
    Library lib(db);
    auto tx = lib.begin();
    lib.upsertCollection({"COLCLASS", "Classics", "", 1});
    lib.upsertCollection({"COLNOVEL", "Novels", "COLCLASS", 1});
    lib.upsertCollection({"COLPOETR", "Poetry", "COLCLASS", 1});
    lib.upsertCollection({"COLSCIEN", "Science", "", 1});
    lib.upsertCollection({"COLPAPER", "Papers", "COLSCIEN", 1});
    lib.upsertCollection({"COLHISTO", "History", "", 1});
    lib.upsertCollection({"COLPOLIT", "Political philosophy", "COLHISTO", 1});
    int64_t clock = 0;
    int n = 0;
    for (const Book& b : kBooks) {
        ItemRecord r;
        r.key = b.key;
        r.version = 1;
        r.itemType = b.type;
        r.title = b.title;
        r.creators.push_back({b.first, b.last, "", "author"});
        r.date = r.year = b.year;
        char added[32];
        std::snprintf(added, sizeof added, "2026-10-%02dT12:00:00Z", 1 + n++);
        r.dateAdded = r.dateModified = added;
        for (const char* c : b.collections) r.collections.push_back(c);
        for (const char* t : b.tags) r.tags.push_back(t);
        r.json = "{}";
        lib.upsertItem(r);
        if (!b.file) continue;
        AttachmentRecord a;
        a.key = std::string(b.key).substr(0, 5) + "EPB";
        a.parentKey = b.key;
        a.contentType = "application/epub+zip";
        a.filename = std::string(b.file) + ".epub";
        a.linkMode = "imported_file";
        lib.upsertAttachment(a);
        const fs::path local = out.parent_path() / "files" / a.filename;
        fs::copy_file("tests/data/epub/" + a.filename, local, fs::copy_options::overwrite_existing);
        lib.setFileState(a.key, AttachmentRecord::State::Local, local.string());
        addHighlights(lib, a.key, b.file, clock);
    }
    tx.commit();
    // Recently read, oldest first; Pride and Prejudice last (Continue reading).
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    const char* read[] = {"GIBBON01", "WASTE001", "ORIGIN01", "MOBY0001", "PRIDE001"};
    const double progress[] = {0.04, 0.6, 0.31, 0.22, 0.13};
    for (int i = 0; i < 5; ++i) {
        const std::string item = read[i];
        lib.markOpened(item, item.substr(0, 5) + "EPB", now - (5 - i) * 3600000LL * 20);
        lib.saveReading(item, "", progress[i]);
    }
    std::printf("demo library: %s\n", out.c_str());
    return 0;
}
