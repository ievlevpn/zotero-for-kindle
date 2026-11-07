#pragma once
// A deterministic synthetic library for screen and query tests (no real data).

#include "store/library.h"

#include <cstdio>
#include <string>

namespace zk::test {

inline std::string fixtureKey(const char* prefix, int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s%06d", prefix, i);
    return buf;
}

// Items I.......: every 3rd has an EPUB, the next a PDF, the third none (every 6th of those a
// linked EPUB, not readable). Creators cycle 3 / 2 / 1 / 0 / 40; years 1962–2025; titles include
// Cyrillic, emoji and empty. Collections: Economics > Growth, and Physics. Tags: economics, pinn,
// to-read. The first 5 EPUBs have 3 annotations each. Plus one standalone PDF "Lonely paper".
inline void fillFixtureLibrary(Library& lib, int items) {
    static const char* titles[] = {"Recursive Macroeconomic Theory", "Философия общего дела", "Owls 🦉 and Graphs", "",
                                   "Physics-informed neural networks", "The Structure of Scientific Revolutions",
                                   "Dynamic Economics", "Algebraic Topology", "A Theory of Justice",
                                   "Numerical Methods in Economics", "Über die Grundlagen", "Deep Learning"};
    static const char* lasts[] = {"Ljungqvist", "Sargent", "Kuhn", "Adda", "Cooper", "Raissi", "Hatcher", "Rawls", "Judd"};
    auto tx = lib.begin();
    lib.upsertCollection({"COLLAAAA", "Economics", "", 1});
    lib.upsertCollection({"COLLBBBB", "Growth", "COLLAAAA", 1});
    lib.upsertCollection({"COLLCCCC", "Physics", "", 1});
    int epubs = 0;
    for (int i = 0; i < items; ++i) {
        ItemRecord r;
        r.key = fixtureKey("IT", i);
        r.version = 1;
        r.itemType = i % 2 ? "book" : "journalArticle";
        const std::string base = titles[i % 12];
        r.title = i < 12 || base.empty() ? base : base + " " + std::to_string(i);
        const int nCreators = std::initializer_list<int>{3, 2, 1, 0, 40}.begin()[i % 5];
        for (int c = 0; c < nCreators; ++c) r.creators.push_back({"A.", lasts[(i + c) % 9], "", "author"});
        r.year = std::to_string(1962 + (i * 7) % 64);
        r.date = r.year;
        char added[32];
        std::snprintf(added, sizeof added, "2024-%02d-%02dT00:00:%02dZ", 1 + (i / 28) % 12, 1 + i % 28, i % 60);
        r.dateAdded = std::to_string(2000 + i / 336) + std::string(added).substr(4);
        r.dateModified = r.dateAdded;
        if (i % 4 == 0) r.collections.push_back("COLLAAAA");
        if (i % 4 == 1) r.collections.push_back("COLLBBBB");
        if (i % 7 == 0) r.collections.push_back("COLLCCCC");
        if (i % 2 == 0) r.tags.push_back("economics");
        if (i % 3 == 0) r.tags.push_back("pinn");
        if (i % 5 == 0) r.tags.push_back("to-read");
        r.json = "{}";
        lib.upsertItem(r);
        AttachmentRecord a;
        a.parentKey = r.key;
        a.linkMode = "imported_file";
        if (i % 3 == 0) {
            a.key = fixtureKey("EP", i);
            a.contentType = "application/epub+zip";
            a.filename = "book.epub";
            lib.upsertAttachment(a);
            if (epubs++ < 5)
                for (int n = 0; n < 3; ++n) {
                    AnnotationRecord an;
                    an.key = fixtureKey(("N" + std::to_string(n)).c_str(), i);
                    an.parentKey = a.key;
                    an.type = "highlight";
                    an.text = "highlight " + std::to_string(n) + " of item " + std::to_string(i);
                    an.sortIndex = "00001|0000000" + std::to_string(n);
                    char mod[32];
                    std::snprintf(mod, sizeof mod, "2025-01-%02dT10:%02d:00Z", 1 + epubs, n);
                    an.dateModified = mod;
                    lib.upsertAnnotation(an);
                }
        } else if (i % 3 == 1) {
            a.key = fixtureKey("PD", i);
            a.contentType = "application/pdf";
            a.filename = "paper.pdf";
            lib.upsertAttachment(a);
        } else if (i % 6 == 2) {
            a.key = fixtureKey("LK", i);
            a.contentType = "application/epub+zip";
            a.linkMode = "linked_file";
            lib.upsertAttachment(a);
        }
    }
    ItemRecord lone;
    lone.key = "LONE0001";
    lone.itemType = "attachment";
    lone.title = "Lonely paper";
    lone.dateAdded = "1999-01-01T00:00:00Z";
    lone.json = "{}";
    lib.upsertItem(lone);
    lib.upsertAttachment({"LONE0001", "", "application/pdf", "lonely.pdf", "", "imported_file", 0, "",
                          AttachmentRecord::State::Remote});
    tx.commit();
}

}  // namespace zk::test
