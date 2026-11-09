#pragma once
// Identifiers of library items (DOI, arXiv id, ISBN, citation key) as plugins see them, and the
// Item JSON every part of `zk` uses (zk.d.ts: Item). Pure functions, no I/O.
//
// An ItemRecord's `json` holds Zotero's raw data object; the identifiers are read from its fields,
// from the Extra field and from URLs. A value that isn't there is "" (an Item shows null).

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "store/records.h"

namespace zk::plugins {

// A DOI as written anywhere: "https://doi.org/10.1000/abc", "doi:10.1000/abc", "DOI: 10.1000/abc.".
// Returns the bare DOI ("10.1000/abc", case kept), or "" if `text` holds none. Trailing punctuation
// (and unbalanced closing brackets) is dropped.
std::string normaliseDoi(const std::string& text);
// What DOIs are compared by: normaliseDoi, lower-cased ("" if none).
std::string doiKey(const std::string& text);

// An arXiv id from "2401.01234v3", "arXiv:2401.01234v3 [cs.LG]", "hep-th/9901001v2" or an
// arxiv.org/abs/ or /pdf/ URL: the id without its version ("2401.01234", "hep-th/9901001"); "" if
// `text` isn't one. Plain text must start with the id; URLs must be arxiv.org's.
std::string normaliseArxiv(const std::string& text);

// An ISBN-10 or ISBN-13 (hyphens and spaces allowed) as 13 digits; "" if it isn't one.
std::string normaliseIsbn(const std::string& text);
// Every ISBN found in a field (Zotero allows several, separated by spaces, commas or semicolons),
// as 13 digits each.
std::vector<std::string> isbnKeys(const std::string& text);

// Everything an item's JSON says about its identifiers, read in one parse.
struct Identifiers {
    std::string doi, arxiv, isbn /* the field as entered */, citationKey;
};
Identifiers identifiersOf(const ItemRecord&);
std::string doiOf(const ItemRecord&);          // the DOI, no prefix
std::string arxivIdOf(const ItemRecord&);      // the arXiv id, no version
std::string citationKeyOf(const ItemRecord&);  // Zotero's citationKey, else Better BibTeX's "Citation Key:" in Extra

// The Item of zk.d.ts: { key, type, title, creators, year, date, publication, doi, arxiv, isbn,
// url, abstract, tags, collections, read, extra, citationKey }. Empty fields are null.
// Keys come out in that order (an ordered_json), which is what a plugin sees in JSON.stringify.
nlohmann::ordered_json itemJson(const ItemRecord&, bool read);

}  // namespace zk::plugins
