#include "docs/epub/cfi.h"

#include <climits>
#include <cstdint>

namespace zk {

namespace {

// ---- text helpers (UTF-16 offsets, as JavaScript counts them) ----

std::u32string utf8to32(const char* s) {
    std::u32string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        char32_t c;
        if (*p < 0x80) {
            c = *p++;
        } else if ((*p >> 5) == 6 && p[1]) {
            c = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if ((*p >> 4) == 14 && p[1] && p[2]) {
            c = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
        } else if (p[1] && p[2] && p[3]) {
            c = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            p += 4;
        } else {
            break;  // truncated sequence
        }
        out += c;
    }
    return out;
}

std::u32string cre32(const lString32& s) { return std::u32string(s.c_str(), s.length()); }

int utf16len(const std::u32string& s, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from; i < to && i < s.size(); ++i) n += s[i] > 0xFFFF ? 2 : 1;
    return n;
}

size_t fromUtf16(const std::u32string& s, int off16) {
    size_t i = 0;
    int n = 0;
    while (i < s.size() && n < off16) n += s[i++] > 0xFFFF ? 2 : 1;
    return i;
}

bool isWs(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0xFEFF; }

// Index in the original text -> index in crengine's text (crengine collapses whitespace).
size_t origIndexToCre(const std::u32string& orig, const std::u32string& cre, size_t origIdx) {
    size_t i = 0, j = 0;
    while (i < origIdx && j < cre.size()) {
        if (cre[j] == orig[i] || (isWs(cre[j]) && isWs(orig[i]))) {
            ++i;
            ++j;
        } else if (isWs(orig[i])) {
            ++i;
        } else {
            ++j;
        }
    }
    return j;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool isTextNode(pugi::xml_node n) { return n.type() == pugi::node_pcdata || n.type() == pugi::node_cdata; }

// crengine children with boxing wrappers flattened.
void creChildren(ldomNode* n, std::vector<ldomNode*>& out) {
    for (int i = 0; i < static_cast<int>(n->getChildCount()); ++i) {
        ldomNode* c = n->getChildNode(i);
        if (c->isElement() && c->isBoxingNode(true)) creChildren(c, out);
        else out.push_back(c);
    }
}

struct CfiPart {
    std::vector<int> steps;
    int offset = 0;
};

// Parses "epubcfi(/6/N!path[,start,end])"; false on anything malformed.
bool parseCfi(const std::string& cfi, int& section, CfiPart& start, CfiPart& end) {
    if (cfi.size() < 10 || cfi.rfind("epubcfi(", 0) != 0 || cfi.back() != ')') return false;
    const std::string s = cfi.substr(8, cfi.size() - 9);
    std::string clean;
    int depth = 0;
    for (char c : s) {
        if (c == '[') ++depth;
        else if (c == ']') --depth;
        else if (!depth) clean += c;
    }
    const size_t bang = clean.find('!');
    if (bang == std::string::npos) return false;
    const std::string pkg = clean.substr(0, bang), rest = clean.substr(bang + 1);
    auto number = [](const std::string& t, int& out) {
        if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos || t.size() > 9) return false;
        out = std::stoi(t);
        return true;
    };
    int spineStep = 0;
    if (!number(pkg.substr(pkg.rfind('/') + 1), spineStep) || spineStep < 2 || spineStep % 2) return false;
    section = spineStep / 2 - 1;
    auto parsePart = [&](const std::string& p, CfiPart& out) {
        const size_t colon = p.find(':');
        const std::string steps = colon == std::string::npos ? p : p.substr(0, colon);
        if (colon != std::string::npos && !number(p.substr(colon + 1), out.offset)) return false;
        size_t i = 0;
        while (i < steps.size()) {
            if (steps[i] != '/') return false;
            const size_t j = steps.find('/', i + 1);
            int st = 0;
            if (!number(steps.substr(i + 1, (j == std::string::npos ? steps.size() : j) - i - 1), st)) return false;
            out.steps.push_back(st);
            if (j == std::string::npos) break;
            i = j;
        }
        return true;
    };
    std::vector<std::string> parts;
    size_t p0 = 0, p1;
    while ((p1 = rest.find(',', p0)) != std::string::npos) {
        parts.push_back(rest.substr(p0, p1 - p0));
        p0 = p1 + 1;
    }
    parts.push_back(rest.substr(p0));
    CfiPart base;
    if (!parsePart(parts[0], base)) return false;
    if (parts.size() == 3) {
        CfiPart a, b;
        if (!parsePart(parts[1], a) || !parsePart(parts[2], b)) return false;
        start = end = base;
        start.steps.insert(start.steps.end(), a.steps.begin(), a.steps.end());
        start.offset = a.offset;
        end.steps.insert(end.steps.end(), b.steps.begin(), b.steps.end());
        end.offset = b.offset;
    } else if (parts.size() == 1) {
        start = end = base;
    } else {
        return false;
    }
    return !start.steps.empty();
}

struct OrigPos {
    pugi::xml_node text;
    int off16 = 0;
};

// Walks steps on the original DOM; the last step must be a text step (odd) — Zotero's
// text steps count text-node siblings (rule 3), offsets are within that node (rule 4).
bool walk(pugi::xml_node html, const CfiPart& part, OrigPos& out) {
    pugi::xml_node n = html;
    for (int st : part.steps) {
        if (st % 2 == 0) {
            const int want = st / 2 - 1;
            int idx = 0;
            pugi::xml_node found;
            for (pugi::xml_node c : n.children())
                if (c.type() == pugi::node_element && idx++ == want) {
                    found = c;
                    break;
                }
            if (!found) return false;
            n = found;
        } else {
            const int want = (st - 1) / 2;
            int idx = 0;
            for (pugi::xml_node c : n.children()) {
                if (!isTextNode(c) || idx++ != want) continue;
                const int len = utf16len(utf8to32(c.value()), 0, SIZE_MAX);
                out = {c, part.offset < len ? part.offset : len};
                return true;
            }
            return false;
        }
    }
    return false;
}

pugi::xml_node nextInDocument(pugi::xml_node n) {
    if (n.first_child()) return n.first_child();
    while (n && !n.next_sibling()) n = n.parent();
    return n ? n.next_sibling() : pugi::xml_node();
}

pugi::xml_node prevInDocument(pugi::xml_node n) {
    if (pugi::xml_node p = n.previous_sibling()) {
        while (p.last_child()) p = p.last_child();
        return p;
    }
    return n.parent();
}

}  // namespace

struct EpubDom::Section {
    ldomNode* creBody = nullptr;
    pugi::xml_document doc;
    pugi::xml_node html;
    std::map<pugi::xml_node_struct*, ldomNode*> origToCre;

    void pair(ldomNode* cre, pugi::xml_node orig) {
        origToCre[orig.internal_object()] = cre;
        if (!cre->isElement()) return;
        std::vector<ldomNode*> cc;
        creChildren(cre, cc);
        std::vector<pugi::xml_node> oc;
        for (pugi::xml_node c : orig.children())
            if (c.type() == pugi::node_element || isTextNode(c)) oc.push_back(c);
        size_t i = 0, j = 0;
        while (i < cc.size() && j < oc.size()) {
            const bool creText = cc[i]->isText(), origText = isTextNode(oc[j]);
            if (creText == origText) {
                pair(cc[i], oc[j]);
                ++i;
                ++j;
            } else if (origText) {
                ++j;  // whitespace-only text crengine dropped
            } else {
                ++i;  // crengine-only node
            }
        }
    }

    // Original position -> crengine position; positions in whitespace crengine dropped snap
    // forward (range start) or back (range end) to the nearest kept text (rule 7).
    bool toCre(OrigPos p, ldomXPointer& out, bool isEnd) {
        const std::u32string orig = utf8to32(p.text.value());
        auto it = origToCre.find(p.text.internal_object());
        if (it != origToCre.end() && it->second->isText()) {
            const std::u32string cre = cre32(it->second->getText());
            out = ldomXPointer(it->second, static_cast<int>(origIndexToCre(orig, cre, fromUtf16(orig, p.off16))));
            return true;
        }
        for (pugi::xml_node n = isEnd ? prevInDocument(p.text) : nextInDocument(p.text); n;
             n = isEnd ? prevInDocument(n) : nextInDocument(n)) {
            if (!isTextNode(n)) continue;
            auto f = origToCre.find(n.internal_object());
            if (f == origToCre.end() || !f->second->isText()) continue;
            out = ldomXPointer(f->second, isEnd ? static_cast<int>(f->second->getText().length()) : 0);
            return true;
        }
        return false;
    }
};

EpubDom::EpubDom(ldomDocument* doc, const std::string& epubPath) : doc_(doc) {
    LVStreamRef f = LVOpenFileStream(epubPath.c_str(), LVOM_READ);
    if (f.isNull()) return;
    arc_ = LVOpenArchieve(f);
    if (arc_.isNull()) return;
    pugi::xml_document container;
    container.load_string(readEntry("META-INF/container.xml").c_str());
    const std::string opfPath =
        container.select_node("//*[local-name()='rootfile']").node().attribute("full-path").value();
    const std::string opfDir = opfPath.find('/') == std::string::npos ? "" : opfPath.substr(0, opfPath.rfind('/') + 1);
    pugi::xml_document opf;
    opf.load_string(readEntry(opfPath).c_str());
    std::map<std::string, std::string> manifest;
    for (auto n : opf.select_nodes("//*[local-name()='manifest']/*[local-name()='item']"))
        manifest[n.node().attribute("id").value()] = n.node().attribute("href").value();
    for (auto n : opf.select_nodes("//*[local-name()='spine']/*[local-name()='itemref']"))
        spine_.push_back(opfDir + urlDecode(manifest[n.node().attribute("idref").value()]));
}

EpubDom::~EpubDom() = default;

std::string EpubDom::readEntry(const std::string& path) {
    if (arc_.isNull()) return {};
    LVStreamRef s = arc_->OpenStream(Utf8ToUnicode(lString8(path.c_str())).c_str(), LVOM_READ);
    if (s.isNull()) return {};
    std::string data(static_cast<size_t>(s->GetSize()), '\0');
    lvsize_t got = 0;
    s->Read(data.data(), data.size(), &got);
    data.resize(got);
    return data;
}

EpubDom::Section* EpubDom::section(int idx) {
    if (idx < 0 || idx >= static_cast<int>(spine_.size()) || !doc_) return nullptr;
    auto it = sections_.find(idx);
    if (it != sections_.end()) return it->second.get();
    // Rule 1: one DocFragment per spine item, in spine order.
    const lString32 path = lString32("/body/DocFragment[") + lString32::itoa(idx + 1) + "]/body";
    ldomXPointer xp = doc_->createXPointer(path);
    if (xp.isNull()) return nullptr;
    auto s = std::make_unique<Section>();
    s->creBody = xp.getNode();
    s->doc.load_string(readEntry(spine_[idx]).c_str(), pugi::parse_default | pugi::parse_ws_pcdata);
    s->html = s->doc.document_element();
    pugi::xml_node body = s->html.child("body");
    if (!body) return nullptr;
    s->pair(s->creBody, body);
    return (sections_[idx] = std::move(s)).get();
}

std::optional<CfiRange> EpubDom::resolve(const std::string& cfi) {
    try {
        int sec = 0;
        CfiPart ps, pe;
        if (!parseCfi(cfi, sec, ps, pe)) return std::nullopt;
        Section* s = section(sec);
        if (!s) return std::nullopt;
        OrigPos oa, ob;
        CfiRange r;
        r.spineIndex = sec;
        if (!walk(s->html, ps, oa) || !walk(s->html, pe, ob)) return std::nullopt;
        if (!s->toCre(oa, r.start, false) || !s->toCre(ob, r.end, true)) return std::nullopt;
        return r;
    } catch (...) {
        return std::nullopt;
    }
}

std::string EpubDom::rangeText(const CfiRange& r) {
    ldomXRange range(r.start, r.end);
    return UnicodeToUtf8(range.getRangeText()).c_str();
}

}  // namespace zk
