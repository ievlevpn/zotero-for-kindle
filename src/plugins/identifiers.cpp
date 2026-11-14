#include "plugins/identifiers.h"

#include <algorithm>
#include <cctype>

namespace zk::plugins {

namespace {

using nlohmann::json;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool startsWithNoCase(const std::string& s, size_t pos, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i)
        if (pos + i >= s.size() || std::tolower(static_cast<unsigned char>(s[pos + i])) != prefix[i]) return false;
    return true;
}

// Skips "http(s)://" and an optional "www."; npos if `s` isn't a URL.
size_t afterScheme(const std::string& s) {
    size_t p = 0;
    if (startsWithNoCase(s, 0, "https://")) p = 8;
    else if (startsWithNoCase(s, 0, "http://")) p = 7;
    else return std::string::npos;
    if (startsWithNoCase(s, p, "www.")) p += 4;
    return p;
}

std::string str(const json& d, const char* key) {
    auto it = d.find(key);
    return it != d.end() && it->is_string() ? it->get<std::string>() : std::string();
}

json parseData(const ItemRecord& r) {
    json d = json::parse(r.json, nullptr, false);
    return d.is_object() ? d : json::object();
}

// The value of the first line of `extra` that starts (ignoring case) with `label` (e.g. "DOI:").
std::string extraLine(const std::string& extra, const char* label) {
    size_t pos = 0;
    while (pos <= extra.size()) {
        size_t end = extra.find('\n', pos);
        if (end == std::string::npos) end = extra.size();
        std::string line = trim(extra.substr(pos, end - pos));
        if (startsWithNoCase(line, 0, label)) return trim(line.substr(std::string(label).size()));
        pos = end + 1;
    }
    return "";
}

char closerOf(char c) { return c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : c == '>' ? '<' : 0; }

}  // namespace

std::string normaliseDoi(const std::string& text) {
    std::string s = trim(text);
    // Leading junk: "(", quotes, "<".
    while (!s.empty() && (s[0] == '(' || s[0] == '"' || s[0] == '\'' || s[0] == '<' || s[0] == '[')) s.erase(0, 1);
    if (size_t p = afterScheme(s); p != std::string::npos) {
        if (startsWithNoCase(s, p, "dx.doi.org/")) s.erase(0, p + 11);
        else if (startsWithNoCase(s, p, "doi.org/")) s.erase(0, p + 8);
    } else if (startsWithNoCase(s, 0, "doi.org/")) {
        s.erase(0, 8);
    } else if (startsWithNoCase(s, 0, "doi:")) {
        s.erase(0, 4);
        s = trim(s);
    }
    if (s.compare(0, 3, "10.") != 0) return "";
    for (size_t i = 0; i < s.size(); ++i)
        if (isSpace(s[i])) {
            s.resize(i);
            break;
        }
    // Trailing punctuation; a closing bracket stays if its opener is inside the DOI.
    while (!s.empty()) {
        const char c = s.back();
        bool strip = c == '.' || c == ',' || c == ';' || c == ':' || c == '"' || c == '\'';
        if (!strip && closerOf(c)) {
            const char open = closerOf(c);
            const auto opens = std::count(s.begin(), s.end(), open), closes = std::count(s.begin(), s.end(), c);
            strip = closes > opens;
        }
        if (!strip) break;
        s.pop_back();
    }
    // "10." must be followed by a registrant and a suffix.
    const size_t slash = s.find('/');
    if (slash == std::string::npos || slash <= 3 || slash + 1 >= s.size()) return "";
    return s;
}

std::string doiKey(const std::string& text) { return lower(normaliseDoi(text)); }

std::string normaliseArxiv(const std::string& text) {
    std::string s = trim(text);
    if (size_t p = afterScheme(s); p != std::string::npos) {
        if (!startsWithNoCase(s, p, "arxiv.org/")) return "";
        p += 10;
        if (startsWithNoCase(s, p, "abs/")) p += 4;
        else if (startsWithNoCase(s, p, "pdf/")) p += 4;
        else if (startsWithNoCase(s, p, "html/")) p += 5;
        else return "";
        s.erase(0, p);
    } else if (startsWithNoCase(s, 0, "arxiv:")) {
        s = trim(s.substr(6));
    }
    // New style: YYMM.NNNN or YYMM.NNNNN. Old style: archive[.XX]/YYMMNNN.
    size_t i = 0;
    std::string id;
    if (s.size() >= 9 && isDigit(s[0]) && isDigit(s[1]) && isDigit(s[2]) && isDigit(s[3]) && s[4] == '.') {
        i = 5;
        while (i < s.size() && isDigit(s[i])) ++i;
        const size_t digits = i - 5;
        if (digits != 4 && digits != 5) return "";
        id = s.substr(0, i);
    } else {
        size_t k = 0;
        while (k < s.size() && (std::islower(static_cast<unsigned char>(s[k])) || s[k] == '-')) ++k;
        if (k == 0) return "";
        if (k < s.size() && s[k] == '.') {  // a subject class: .GT, .AG
            size_t c = k + 1;
            while (c < s.size() && std::isalpha(static_cast<unsigned char>(s[c]))) ++c;
            if (c == k + 1) return "";
            k = c;
        }
        if (k >= s.size() || s[k] != '/') return "";
        size_t d = k + 1;
        while (d < s.size() && isDigit(s[d])) ++d;
        if (d - (k + 1) != 7) return "";
        i = d;
        id = s.substr(0, i);
    }
    // A version, then the end of the id: the end of the text or a character that can't continue it.
    if (i < s.size() && s[i] == 'v') {
        size_t v = i + 1;
        while (v < s.size() && isDigit(s[v])) ++v;
        if (v == i + 1) return "";
        i = v;
    }
    if (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '/')) return "";
    return id;
}

std::string normaliseIsbn(const std::string& text) {
    std::string d;
    for (char c : text) {
        if (isDigit(c)) d += c;
        else if ((c == 'x' || c == 'X') && d.size() == 9) d += 'X';
        else if (c == '-' || isSpace(c)) continue;
        else if (std::isalpha(static_cast<unsigned char>(c)) && d.empty()) continue;  // "ISBN"
        else return "";
    }
    if (d.size() == 13) {
        if (d.find('X') != std::string::npos) return "";
        return d;
    }
    if (d.size() != 10) return "";
    std::string core = "978" + d.substr(0, 9);
    if (core.find('X') != std::string::npos) return "";
    int sum = 0;
    for (size_t i = 0; i < 12; ++i) sum += (core[i] - '0') * (i % 2 ? 3 : 1);
    core += static_cast<char>('0' + (10 - sum % 10) % 10);
    return core;
}

std::vector<std::string> isbnKeys(const std::string& text) {
    // A run of digits, hyphens and X (spaces inside a run are split: "0198526636 9780198526636").
    std::vector<std::string> out;
    std::string run;
    auto flush = [&] {
        if (!run.empty()) {
            std::string k = normaliseIsbn(run);
            if (!k.empty() && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
        }
        run.clear();
    };
    for (char c : text) {
        if (isDigit(c) || c == '-' || ((c == 'x' || c == 'X') && !run.empty())) run += c;
        else flush();
    }
    flush();
    return out;
}

Identifiers identifiersOf(const ItemRecord& r) {
    const json d = parseData(r);
    Identifiers id;
    const std::string extra = str(d, "extra"), url = str(d, "url");

    id.doi = normaliseDoi(str(d, "DOI"));
    if (id.doi.empty()) id.doi = normaliseDoi(url);  // only a doi.org URL gives one
    if (id.doi.empty()) id.doi = normaliseDoi(extraLine(extra, "doi:"));

    for (const std::string& candidate : {str(d, "archiveID"), extraLine(extra, "arxiv:"), url}) {
        if (candidate.empty()) continue;
        id.arxiv = normaliseArxiv(candidate);
        if (!id.arxiv.empty()) break;
    }
    if (id.arxiv.empty() && startsWithNoCase(id.doi, 0, "10.48550/arxiv.")) id.arxiv = normaliseArxiv(id.doi.substr(15));
    if (id.arxiv.empty()) id.arxiv = normaliseArxiv(extraLine(extra, "arxiv id:"));

    id.isbn = trim(str(d, "ISBN"));
    id.citationKey = trim(str(d, "citationKey"));
    if (id.citationKey.empty()) {
        std::string key = extraLine(extra, "citation key:");
        key = key.substr(0, key.find_first_of(" \t"));
        id.citationKey = key;
    }
    return id;
}

std::string doiOf(const ItemRecord& r) { return identifiersOf(r).doi; }
std::string arxivIdOf(const ItemRecord& r) { return identifiersOf(r).arxiv; }
std::string citationKeyOf(const ItemRecord& r) { return identifiersOf(r).citationKey; }

nlohmann::ordered_json itemJson(const ItemRecord& r, bool read, const std::string& libraryName) {
    using json = nlohmann::ordered_json;
    const nlohmann::json d = parseData(r);
    const Identifiers id = identifiersOf(r);
    auto orNull = [](const std::string& s) { return s.empty() ? json(nullptr) : json(s); };
    json creators = json::array();
    for (const auto& c : r.creators) {
        json o = json::object();
        if (!c.first.empty()) o["first"] = c.first;
        if (!c.last.empty()) o["last"] = c.last;
        if (!c.name.empty()) o["name"] = c.name;
        o["type"] = c.type;
        creators.push_back(std::move(o));
    }
    json year = nullptr;
    if (!r.year.empty() && std::all_of(r.year.begin(), r.year.end(), isDigit) && r.year.size() <= 9) year = std::stoi(r.year);
    json j = json::object();
    j["key"] = r.key;
    j["type"] = r.itemType;
    j["title"] = r.title;
    j["creators"] = std::move(creators);
    j["year"] = std::move(year);
    j["date"] = orNull(r.date);
    j["publication"] = orNull(r.publication);
    j["doi"] = orNull(id.doi);
    j["arxiv"] = orNull(id.arxiv);
    j["isbn"] = orNull(id.isbn);
    j["url"] = orNull(trim(str(d, "url")));
    j["abstract"] = orNull(str(d, "abstractNote"));
    j["tags"] = r.tags;
    j["collections"] = r.collections;
    j["read"] = read;
    j["extra"] = orNull(str(d, "extra"));
    j["citationKey"] = orNull(id.citationKey);
    j["libraryId"] = r.lib;
    j["libraryName"] = libraryName;
    return j;
}

}  // namespace zk::plugins
