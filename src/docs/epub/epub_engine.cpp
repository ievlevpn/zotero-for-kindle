#include "docs/epub/epub_engine.h"
#include "docs/fonts.h"

#include <crengine.h>

#include <dirent.h>
#include <sys/stat.h>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include <algorithm>
#include <regex>
#include <fstream>
#include <set>
#include <mutex>
#include <sstream>

#include "docs/epub/cfi.h"

namespace zk {

namespace {

constexpr double kDpi = 300.0;

std::string utf8(const lString32& s) { return UnicodeToUtf8(s).c_str(); }

// crengine is built without its own locking (crlocks.h is off) and its fonts are process-wide:
// two books on two render workers (one closing, the next opening) must take turns, or one's
// shaping switch frees glyphs the other is drawing with.
std::recursive_mutex& crengineMutex() {
    static std::recursive_mutex m;
    return m;
}

// crengine's font manager and hyphenation are process-wide: set them up once. Font folders are
// registered the first time an engine names them.
void registerFonts(const std::vector<std::string>& dirs) {
    static std::mutex m;
    static std::set<std::string> done;
    std::lock_guard<std::mutex> lock(m);
    for (const auto& dir : dirs) {
        if (!done.insert(dir).second) continue;
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            const std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
            if (ext == ".ttf" || ext == ".otf" || ext == ".TTF" || ext == ".OTF")
                fontMan->RegisterFont(lString8((dir + "/" + name).c_str()));
        }
        closedir(d);
    }
}

void initCrengine(const EngineEnv& env) {
    static std::once_flag once;
    std::call_once(once, [&] {
        InitFontManager(lString8::empty_str);
        // The trailing slash matters: crengine treats a path without one as a single file
        // (why spike S1's dictionaries never loaded).
        HyphMan::initDictionaries(Utf8ToUnicode(lString8((env.dataDir + "/hyph/").c_str())));
        HyphMan::activateDictionary(U"English_US.pattern");  // books without a language
        TextLangMan::setHyphenationEnabled(true);
        TextLangMan::setEmbeddedLangsEnabled(true);
        if (!env.cacheDir.empty()) {
            ::mkdir(env.cacheDir.c_str(), 0755);
            ldomDocCache::init(Utf8ToUnicode(lString8(env.cacheDir.c_str())), 64 * 1024 * 1024);
        }
    });
    registerFonts(env.fontDirs);
}

// META-INF/encryption.xml that encrypts more than fonts = DRM.
bool hasDrm(const std::string& path) {
    LVStreamRef f = LVOpenFileStream(path.c_str(), LVOM_READ);
    if (f.isNull()) return false;
    LVContainerRef arc = LVOpenArchieve(f);
    if (arc.isNull()) return false;
    LVStreamRef enc = arc->OpenStream(U"META-INF/encryption.xml", LVOM_READ);
    if (enc.isNull()) return false;
    std::string xml(static_cast<size_t>(enc->GetSize()), '\0');
    lvsize_t got = 0;
    enc->Read(xml.data(), xml.size(), &got);
    xml.resize(got);
    // Font obfuscation (IDPF / Adobe) is allowed; anything else means the content is encrypted.
    size_t pos = 0;
    while ((pos = xml.find("Algorithm=\"", pos)) != std::string::npos) {
        pos += 11;
        const std::string alg = xml.substr(pos, xml.find('"', pos) - pos);
        if (alg != "http://www.idpf.org/2008/embedding" && alg != "http://ns.adobe.com/pdf/enc#RC") return true;
    }
    return false;
}

bool looksLikeZip(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char sig[4] = {0};
    in.read(sig, 4);
    return in.gcount() == 4 && sig[0] == 'P' && sig[1] == 'K';
}

}  // namespace

EpubEngine::EpubEngine(const EngineEnv& env) : env_(env) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    initCrengine(env_);
}

EpubEngine::~EpubEngine() {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    dom_.reset();  // holds pointers into the view's document
    links_.clear();
    view_.reset();  // the document and its fonts go inside the lock, not after it
}

void EpubEngine::applyLayout() {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_) return;
    // Fast Font bolds word beginnings with an OpenType feature: it needs HarfBuzz shaping, which
    // costs time, so only while one is chosen.
    const kerning_mode_t kerning = isFastFont(layout_.font) ? KERNING_MODE_HARFBUZZ : KERNING_MODE_DISABLED;
    if (fontMan->GetKerningMode() != kerning) fontMan->SetKerningMode(kerning);
    view_->setDefaultFontFace(lString8(layout_.font.c_str()));
    // The chosen font wins over the ones a book's style sheet names (most EPUBs name some:
    // "Times New Roman", Arial, embedded fonts), as on the Kindle's own reader; code stays
    // monospace.
    std::string family;
    for (char ch : layout_.font)
        if (ch != '"' && ch != '\\') family += ch;
    view_->setStyleSheet(lString8((baseCss_ + "\nbody, body * { font-family: \"" + family +
                                   "\" !important; }\npre, pre *, code, code *, tt, kbd, samp { font-family: monospace !important; }\n")
                                      .c_str()));
    view_->setFontSize(static_cast<int>(layout_.sizePt * kDpi / 72.0 + 0.5));
    view_->setPageMargins(lvRect(layout_.marginPx, layout_.marginPx, layout_.marginPx, layout_.marginPx / 2));
    view_->setPageHeaderInfo(0);
    view_->setDefaultInterlineSpace(layout_.lineSpacingPct);
    view_->setViewMode(DVM_PAGES, cols());  // two pages side by side in landscape
    view_->Resize(layout_.width, layout_.height);
}

// crengine settles its position lazily (back to page 0) on the first query after a render.
void EpubEngine::settle() {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    view_->checkRender();
    view_->getCurPage();
    toc_.clear();
}

bool EpubEngine::open(const std::string& path, UserError* err) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    auto fail = [&](const char* msg, const char* detail) {
        view_.reset();
        if (err) *err = UserError{ErrorKind::Other, msg, detail};
        return false;
    };
    if (::access(path.c_str(), R_OK) != 0)
        return fail("This book's file is missing. Open it from the library to download it again.", path.c_str());
    if (!looksLikeZip(path)) return fail("This EPUB is damaged and can't be opened.", "not a zip file");
    if (hasDrm(path)) return fail("This EPUB is protected (DRM) and can't be opened.", "META-INF/encryption.xml");
    view_ = std::make_unique<LVDocView>(8);
    std::ifstream css(env_.dataDir + "/epub.css");
    std::stringstream cssText;
    cssText << css.rdbuf();
    baseCss_ = cssText.str();
    applyLayout();  // the style sheet, with the chosen font
    if (!view_->LoadDocument(path.c_str()) || !view_->getDocument())
        return fail("This EPUB is damaged and can't be opened.", "crengine: LoadDocument failed");
    view_->setViewMode(DVM_PAGES, cols());
    settle();
    if (view_->getPageCount() <= 0) return fail("This EPUB is damaged and can't be opened.", "no pages");
    dom_ = std::make_unique<EpubDom>(view_->getDocument(), path);
    return true;
}

void EpubEngine::setLayout(const Layout& l) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    linksPage_ = -1;  // laid out again: the cached links' boxes are gone
    if (l == layout_ && view_) return;
    layout_ = l;
    applyLayout();
    if (view_) settle();
}

int EpubEngine::pageCount() {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    return view_ ? (view_->getPageCount() + cols() - 1) / cols() : 0;
}

PageImage EpubEngine::render(int page) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    PageImage img;
    if (!view_) return img;
    view_->goToPage(crPage(page));
    LVGrayDrawBuf buf(layout_.width, layout_.height, 8);
    view_->Draw(buf, false);
    img.width = layout_.width;
    img.height = layout_.height;
    img.gray.resize(static_cast<size_t>(img.width) * img.height);
    for (int y = 0; y < img.height; ++y)
        std::copy_n(buf.GetScanLine(y), img.width, img.gray.begin() + static_cast<size_t>(y) * img.width);
    return img;
}

DocPosition EpubEngine::positionOfPage(int page) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_) return {};
    ldomXPointer p = view_->getPageBookmark(crPage(page));
    return p.isNull() ? DocPosition{} : DocPosition{utf8(p.toString())};
}

const std::vector<EpubEngine::PageLink>& EpubEngine::pageLinks(int page) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (page == linksPage_) return links_;
    links_.clear();
    linksPage_ = page;
    if (!view_ || page < 0 || page >= pageCount()) return links_;
    // One call covers the spread: with two columns crengine's current page is both of them.
    view_->goToPage(crPage(page));
    ldomXRangeList list;
    view_->getCurrentPageLinks(list);
    ldomDocument* doc = view_->getDocument();
    for (int i = 0; i < list.length(); ++i) {
        ldomXRange* r = list[i];
        ldomXPointer a;
        const lString32 href = r->getHRef(a);
        if (href.empty()) continue;
        PageLink link;
        link.anchor = a.getNode();
        link.href = utf8(href);
        link.text = utf8(r->getRangeText());
        if (link.href[0] == '#') {  // only links we can follow
            if (link.href.size() < 2 || !doc->getNodeById(doc->getAttrValueIndex(href.substr(1).c_str()))) continue;
        } else if (link.href.find(':') == std::string::npos) {
            continue;
        }
        bool seen = false;
        for (const auto& l : links_) seen |= l.anchor == link.anchor;
        if (seen) continue;
        LVArray<lvRect> rects;
        r->getSegmentRects(rects);
        for (int k = 0; k < rects.length(); ++k) {
            lvPoint tl = rects[k].topLeft(), br = rects[k].bottomRight();
            if (!view_->docToWindowPoint(tl) || !view_->docToWindowPoint(br, true)) continue;
            const Rect box{tl.x, tl.y, br.x - tl.x, br.y - tl.y};
            if (!box.empty() && box.y >= 0 && box.y + box.h <= layout_.height) link.rects.push_back(box);
        }
        if (!link.rects.empty()) links_.push_back(std::move(link));
    }
    return links_;
}

std::vector<LinkBox> EpubEngine::links(int page) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    std::vector<LinkBox> out;
    const auto& links = pageLinks(page);
    for (size_t i = 0; i < links.size(); ++i)
        for (const Rect& r : links[i].rects) out.push_back({r, static_cast<int>(i)});
    return out;
}

namespace {

bool hasWord(const lString32& value, const char* word) { return utf8(value).find(word) != std::string::npos; }

std::string nameOf(ldomNode* n) { return n && n->isElement() ? utf8(n->getNodeName()) : ""; }

const lString32& attr(ldomNode* n, const char* name) { return n->getAttributeValue((const lChar8*)nullptr, name); }

// epub:type / role marking a note: <a epub:type="noteref">, <aside epub:type="footnote">, …
bool markedNote(ldomNode* n, bool ref) {
    if (!n || !n->isElement()) return false;
    const lString32 &type = attr(n, "type"), &role = attr(n, "role"), &cls = attr(n, "class");
    if (ref) return hasWord(type, "noteref") || hasWord(role, "doc-noteref") || hasWord(cls, "footnote-ref");
    return hasWord(type, "footnote") || hasWord(type, "endnote") || hasWord(type, "rearnote") ||
           hasWord(role, "doc-footnote") || hasWord(role, "doc-endnote") || hasWord(cls, "footnotes");
}

// A link back from a note to the text (never a note itself).
bool backLink(ldomNode* a) {
    if (!a || !a->isElement()) return false;
    const lString32 &type = attr(a, "type"), &role = attr(a, "role"), &cls = attr(a, "class");
    return hasWord(type, "backlink") || hasWord(role, "doc-backlink") || hasWord(cls, "footnote-back");
}

bool structural(const std::string& name) {
    return (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') || name == "section" ||
           name == "body" || name == "DocFragment" || name == "html" || name == "nav";
}

std::string collapsed(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

bool endsWith(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

// A note's text as shown: without the way back ("(return)", "↩"), the repeated mark, brackets.
std::string cleanNote(std::string t, const std::string& mark) {
    t = collapsed(t);
    for (const std::string back : {"( return )", "(return)", "( return)", "(return )"})
        for (size_t at; (at = t.find(back)) != std::string::npos;) t.erase(at, back.size());
    t = collapsed(t);
    if (!mark.empty() && t.rfind(mark + " ", 0) == 0) t = t.substr(mark.size() + 1);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::string back : {"\xE2\x86\xA9\xEF\xB8\x8E", "\xE2\x86\xA9", "\xE2\x86\x91", "^", "[Return]", "Back",
                                       "return to text"})
            if (endsWith(t, back)) {
                t = collapsed(t.substr(0, t.size() - back.size()));
                changed = true;
            }
    }
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') t = collapsed(t.substr(1, t.size() - 2));
    return t;
}

ldomNode* blockOf(ldomNode* n) {
    while (n && n->getParentNode() && n->getRendMethod() == erm_inline) n = n->getParentNode();
    return n;
}

// The next block after `b` in document order (its next sibling, or its parent's), or null.
ldomNode* nextBlock(ldomNode* b) {
    for (ldomNode* n = b; n && n->getParentNode(); n = n->getParentNode()) {
        ldomNode* parent = n->getParentNode();
        for (int i = n->getNodeIndex() + 1; i < (int)parent->getChildCount(); ++i) {
            ldomNode* c = parent->getChildNode(i);
            if (c && c->isElement()) return c;
        }
        if (nameOf(parent) == "DocFragment") return nullptr;  // not into the next chapter
    }
    return nullptr;
}

// Another note starts here: an empty anchor with an id first in the block (Gutenberg).
bool startsNote(ldomNode* b) {
    for (ldomNode* n = b; n && n->isElement() && n->getChildCount() > 0;) {
        ldomNode* first = nullptr;
        for (int i = 0; i < (int)n->getChildCount() && !first; ++i)
            if (n->getChildNode(i)->isElement()) first = n->getChildNode(i);
        if (!first) return false;
        if (nameOf(first) == "a" && !attr(first, "id").empty() && collapsed(utf8(first->getText())).empty()) return true;
        n = first;
    }
    return false;
}

}  // namespace

std::optional<LinkInfo> EpubEngine::link(int page, int index) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    const auto& links = pageLinks(page);
    if (!view_ || index < 0 || index >= static_cast<int>(links.size())) return std::nullopt;
    const PageLink& l = links[static_cast<size_t>(index)];
    LinkInfo info;
    info.mark = collapsed(l.text);
    if (l.href[0] != '#') {
        info.kind = LinkInfo::Kind::Web;
        info.label = "Web link";
        info.url = l.href;
        return info;
    }
    ldomDocument* doc = view_->getDocument();
    const lString32 id = Utf8ToUnicode(lString8(l.href.c_str() + 1));
    ldomNode* dest = doc->getNodeById(doc->getAttrValueIndex(id.c_str()));
    if (!dest) return std::nullopt;
    ldomXPointer target(dest, 0);
    info.target = {utf8(target.toString())};
    info.targetPage = spreadOf(view_->getBookmarkPage(target));
    info.kind = LinkInfo::Kind::Jump;
    // Never a note: a link back from a note, a printed-page anchor, a heading or a whole section.
    // (crengine prefixes ids with their file: a printed-page anchor ends in "page_23", "Page23", …)
    std::string lowered = l.href.substr(1);
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::regex pageAnchor(R"((^|[^a-z])page[_\-.]?[0-9ivxlc]+$)");
    ldomNode* block = blockOf(dest);
    if (backLink(l.anchor) || std::regex_search(lowered, pageAnchor) || structural(nameOf(dest)) || structural(nameOf(block)))
        return info;
    bool note = markedNote(l.anchor, true);
    for (ldomNode* n = dest; n && !note; n = n->getParentNode()) {
        note = markedNote(n, false);
        if (nameOf(n) == "DocFragment") break;
    }
    // The note's text: from the target on, to the end of its block; if nothing is left there
    // (Gutenberg's empty anchor paragraph), the next block, unless another note starts there.
    std::string text;
    {
        ldomXRange whole(block, true);
        const ldomXPointer start(dest, 0), end = whole.getEnd();
        const std::string from = collapsed(utf8(ldomXRange(start, end).getRangeText(' ')));
        text = from;
        ldomNode* b = block;
        for (int hops = 0; text.empty() && hops < 2; ++hops) {
            b = nextBlock(b);
            if (!b || structural(nameOf(b)) || startsNote(b)) break;
            text = collapsed(utf8(b->getText(' ')));
        }
    }
    // Unmarked: a short mark ("3", "*", "†") pointing at a short passage is a footnote too.
    if (!note && info.mark.size() <= 4 && !text.empty() && text.size() < 3000) note = true;
    if (!note) return info;
    info.kind = LinkInfo::Kind::Note;
    info.label = info.mark.empty() || info.mark.size() > 8 ? "Footnote" : "Footnote " + info.mark;
    info.text = cleanNote(text, info.mark);
    return info;
}

int EpubEngine::pageOfPlaced(const DocPosition& pos) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_ || pos.value.empty()) return -1;
    ldomXPointer p = view_->getDocument()->createXPointer(Utf8ToUnicode(lString8(pos.value.c_str())));
    if (p.isNull()) return -1;
    const int page = spreadOf(view_->getBookmarkPage(p));
    return page >= 0 && page < pageCount() ? page : -1;
}

int EpubEngine::pageOf(const DocPosition& pos) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_ || pos.value.empty()) return 0;
    ldomXPointer p = view_->getDocument()->createXPointer(Utf8ToUnicode(lString8(pos.value.c_str())));
    if (p.isNull()) return 0;
    const int page = spreadOf(view_->getBookmarkPage(p));
    return page < 0 ? 0 : page;
}

std::vector<TocEntry> EpubEngine::toc() {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_) return {};
    if (!toc_.empty()) return toc_;
    LVTocItem* root = view_->getToc();
    if (!root) return {};
    view_->updatePageNumbers(root);
    std::function<void(LVTocItem*)> walk = [&](LVTocItem* item) {
        for (int i = 0; i < item->getChildCount(); ++i) {
            LVTocItem* c = item->getChild(i);
            ldomXPointer xp = c->getXPointer();
            toc_.push_back({utf8(c->getName()), c->getLevel() - 1, {xp.isNull() ? std::string{} : utf8(xp.toString())},
                            spreadOf(c->getPage())});
            walk(c);
        }
    };
    walk(root);
    return toc_;
}

std::string EpubEngine::chapterAt(int page) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    std::string title;
    for (const auto& t : toc())
        if (t.page <= page) title = t.title;
    return title;
}

std::vector<SearchHit> EpubEngine::search(const std::string& query, int maxHits) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    std::vector<SearchHit> out;
    if (!view_ || query.empty()) return out;
    ldomXRangeList ranges;
    // maxHeight 0 = no height limit (a large value overflows crengine's y arithmetic).
    if (!view_->getDocument()->findText(Utf8ToUnicode(lString8(query.c_str())), true, false, 0, 0, ranges, maxHits, 0))
        return out;
    for (int i = 0; i < ranges.length(); ++i) {
        ldomXRange* r = ranges[i];
        ldomXPointer start = r->getStart();
        SearchHit h;
        h.pos = {utf8(start.toString())};
        h.page = spreadOf(view_->getBookmarkPage(start));
        // A snippet of about 40 characters either side, from the paragraph's text.
        const lString32 text = start.getNode() ? start.getNode()->getText() : lString32();
        const int at = start.getOffset();
        const int from = std::max(0, at - 40);
        h.snippet = (from > 0 ? "…" : "") + utf8(text.substr(from, std::min<int>(text.length() - from, at - from + static_cast<int>(query.size()) + 40)));
        out.push_back(std::move(h));
    }
    return out;
}

namespace {

// The CFI inside a Zotero EPUB annotation's position: {"type":"FragmentSelector","value":"epubcfi(…)"}.
std::string cfiOf(const AnnotationRecord& a) {
    auto j = nlohmann::json::parse(a.position, nullptr, false);
    if (!j.is_object() || !j.contains("value") || !j["value"].is_string()) return {};
    return j["value"].get<std::string>();
}

}  // namespace

// Zotero's lastPageIndex for an EPUB: a point CFI at the start of what is shown, without the
// "epubcfi(...)" wrapper. Made from the CFI of the page's first character.
std::string EpubEngine::zoteroPosition(int page) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_ || !dom_ || page < 0 || page >= pageCount()) return "";
    LVRef<ldomXRange> range = view_->getPageDocumentRange(crPage(page));
    if (range.isNull()) return "";
    ldomXPointerEx p(range->getStart());
    if (!p.isText() && !p.nextVisibleText()) return "";
    ldomXPointer start(p.getNode(), p.getOffset());
    const int length = start.getNode()->getText().length();
    if (start.getOffset() >= length) return "";
    const ldomXPointer end(start.getNode(), start.getOffset() + 1);
    auto generated = dom_->generate(start, end);
    if (!generated) return "";
    // "epubcfi(base,/start,/end)" -> "base/start"
    std::string inner = generated->cfi.substr(8, generated->cfi.size() - 9);
    const size_t c1 = inner.find(',');
    if (c1 != std::string::npos) {
        const size_t c2 = inner.find(',', c1 + 1);
        inner = inner.substr(0, c1) + inner.substr(c1 + 1, (c2 == std::string::npos ? inner.size() : c2) - c1 - 1);
    }
    return nlohmann::json(inner).dump();
}

int EpubEngine::pageOfZoteroPosition(const std::string& v) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_ || !dom_) return -1;
    const auto j = nlohmann::json::parse(v, nullptr, false);
    if (!j.is_string()) return -1;
    auto r = dom_->resolve("epubcfi(" + j.get<std::string>() + ")");
    if (!r) return -1;
    const int page = spreadOf(view_->getBookmarkPage(r->start));
    return page >= 0 && page < pageCount() ? page : -1;
}

int EpubEngine::pageOfAnnotation(const AnnotationRecord& a) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_ || !dom_) return -1;
    auto r = dom_->resolve(cfiOf(a));
    if (!r) return -1;
    return spreadOf(view_->getBookmarkPage(r->start));
}

std::vector<HighlightBox> EpubEngine::highlights(int page, const std::vector<AnnotationRecord>& anns) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    std::vector<HighlightBox> out;
    if (!view_ || !dom_) return out;
    view_->goToPage(crPage(page));
    for (const auto& a : anns) {
        auto r = dom_->resolve(cfiOf(a));
        if (!r) continue;
        ldomXRange range(r->start, r->end);
        LVArray<lvRect> rects;
        range.getSegmentRects(rects);
        for (int i = 0; i < rects.length(); ++i) {
            lvPoint tl = rects[i].topLeft(), br = rects[i].bottomRight();
            if (!view_->docToWindowPoint(tl) || !view_->docToWindowPoint(br, true)) continue;  // another page
            Rect box{tl.x, tl.y, br.x - tl.x, br.y - tl.y};
            if (box.empty() || box.y < 0 || box.y + box.h > layout_.height) continue;
            out.push_back({a.key, box, shadeOf(a.color), a.type == "underline"});
        }
    }
    return out;
}

int64_t EpubEngine::wordsBetween(int fromPage, int toPage) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_ || toPage <= fromPage) return 0;
    ldomXPointer a = view_->getPageBookmark(crPage(fromPage)),
                 b = view_->getPageBookmark(std::min(crPage(toPage), view_->getPageCount() - 1));
    if (a.isNull() || b.isNull()) return 0;
    const lString32 text = ldomXRange(a, b).getRangeText();
    int64_t words = 0;
    bool inWord = false;
    for (int i = 0; i < text.length(); ++i) {
        const bool space = text[i] == ' ' || text[i] == '\n' || text[i] == '\t';
        if (!space && !inWord) ++words;
        inWord = !space;
    }
    return words;
}

namespace {

// Paragraphs of crengine's range text (one '\n' per block) joined by a blank line: each trimmed,
// empty ones dropped.
void appendParagraphs(const std::string& raw, std::string& out) {
    size_t i = 0;
    while (i <= raw.size()) {
        size_t j = raw.find('\n', i);
        if (j == std::string::npos) j = raw.size();
        size_t a = i, b = j;
        while (a < b && (raw[a] == ' ' || raw[a] == '\t' || raw[a] == '\r')) ++a;
        while (b > a && (raw[b - 1] == ' ' || raw[b - 1] == '\t' || raw[b - 1] == '\r')) --b;
        if (b > a) {
            if (!out.empty()) out += "\n\n";
            out.append(raw, a, b - a);
        }
        i = j + 1;
    }
}

}  // namespace

// The book's chapters (crengine's DocFragments) in order, paragraphs separated by a blank line.
// The crengine lock is taken per chapter, so the reader's render worker can run in between.
std::string EpubEngine::fullText() {
    std::vector<int> fragments;  // child indexes of the root's first element with DocFragments
    ldomNode* holder = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(crengineMutex());
        if (!view_) return {};
        ldomNode* root = view_->getDocument()->getRootNode();
        // Chapters are DocFragment elements, children of <body>; with none, the whole document is one.
        std::vector<ldomNode*> stack{root};
        while (!stack.empty() && !holder) {
            ldomNode* n = stack.back();
            stack.pop_back();
            for (int i = 0; i < (int)n->getChildCount(); ++i) {
                ldomNode* c = n->getChildNode(i);
                if (c && c->isElement() && nameOf(c) == "DocFragment") {
                    holder = n;
                    break;
                }
                if (c && c->isElement()) stack.push_back(c);
            }
        }
        if (holder)
            for (int i = 0; i < (int)holder->getChildCount(); ++i) {
                ldomNode* c = holder->getChildNode(i);
                if (c && c->isElement() && nameOf(c) == "DocFragment") fragments.push_back(i);
            }
    }
    std::string out;
    auto take = [&](ldomNode* n) {
        ldomXRange whole(n, true);
        appendParagraphs(utf8(whole.getRangeText('\n')), out);
    };
    if (!holder) {
        std::lock_guard<std::recursive_mutex> lock(crengineMutex());
        take(view_->getDocument()->getRootNode());
        return out;
    }
    for (int i : fragments) {
        if (out.size() > kMaxFullTextBytes) break;
        std::lock_guard<std::recursive_mutex> lock(crengineMutex());
        if (!view_ || i >= (int)holder->getChildCount()) break;
        take(holder->getChildNode(i));
    }
    return out;
}

namespace {

bool isSpace(lChar32 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0xFEFF; }

lChar32 charAt(const ldomXPointer& p) {
    if (!p.isText()) return 0;
    const lString32 text = p.getNode()->getText();
    return p.getOffset() < text.length() ? text[p.getOffset()] : 0;
}

// Words are runs of non-space characters (punctuation and quotes included, as a reader expects
// "considerable." or “Why to be selected whole), within one text node.
ldomXPointer wordStart(ldomXPointer p) {
    const lString32 text = p.getNode()->getText();
    int i = p.getOffset();
    while (i > 0 && !isSpace(text[i - 1])) --i;
    return ldomXPointer(p.getNode(), i);
}

ldomXPointer wordEnd(ldomXPointer p) {
    const lString32 text = p.getNode()->getText();
    int i = std::min(p.getOffset(), static_cast<int>(text.length()));
    if (i < text.length() && isSpace(text[i])) {  // on a space: end after the previous word
        while (i > 0 && isSpace(text[i - 1])) --i;
        return ldomXPointer(p.getNode(), i);
    }
    while (i < text.length() && !isSpace(text[i])) ++i;
    return ldomXPointer(p.getNode(), i);
}

}  // namespace

std::optional<Selection> EpubEngine::select(int page, Point from, Point to) {
    std::lock_guard<std::recursive_mutex> lock(crengineMutex());
    if (!view_ || !dom_ || page < 0 || page >= pageCount()) return std::nullopt;
    view_->goToPage(crPage(page));
    // The drag stays on this page: a finger over the footer means the page's last line.
    to.y = std::clamp(to.y, 0, layout_.height - layout_.marginPx / 2 - 1);
    // A press in a margin selects nothing (crengine would snap it to the nearest text).
    constexpr int kSlack = 12;
    const int m = layout_.marginPx;
    if (from.x < m - kSlack || from.x > layout_.width - m + kSlack || from.y < m - kSlack ||
        from.y > layout_.height - m / 2 + kSlack)
        return std::nullopt;
    ldomXPointer a = view_->getNodeByPoint(lvPoint(from.x, from.y), true, true);
    if (a.isNull() || !a.isText()) return std::nullopt;
    if (isSpace(charAt(a)) && a.getOffset() > 0 && !isSpace(a.getNode()->getText()[a.getOffset() - 1]))
        a = ldomXPointer(a.getNode(), a.getOffset() - 1);  // just after a word: that word
    if (isSpace(charAt(a))) return std::nullopt;
    ldomXPointer b = view_->getNodeByPoint(lvPoint(to.x, to.y), false, true);
    if (b.isNull() || !b.isText()) b = a;
    ldomXPointerEx first(a), last(b);
    if (last.compare(first) < 0) std::swap(first, last);
    ldomXPointer start = wordStart(first), end = wordEnd(last);
    // The spread's text: from its first page's start to its last page's end.
    LVRef<ldomXRange> firstRange = view_->getPageDocumentRange(crPage(page));
    LVRef<ldomXRange> lastRange = view_->getPageDocumentRange(std::min(crPage(page) + cols() - 1, view_->getPageCount() - 1));
    if (!firstRange.isNull() && ldomXPointerEx(start).compare(ldomXPointerEx(firstRange->getStart())) < 0)
        start = firstRange->getStart();
    if (!lastRange.isNull() && ldomXPointerEx(end).compare(ldomXPointerEx(lastRange->getEnd())) > 0) end = lastRange->getEnd();
    if (ldomXPointerEx(end).compare(ldomXPointerEx(start)) <= 0) return std::nullopt;

    auto generated = dom_->generate(start, end);
    if (!generated || generated->text.empty()) return std::nullopt;
    Selection sel;
    sel.text = generated->text;
    sel.sortIndex = generated->sortIndex;
    sel.position = nlohmann::json{{"type", "FragmentSelector"},
                                  {"conformsTo", "http://www.idpf.org/epub/linking/cfi/epub-cfi.html"},
                                  {"value", generated->cfi}}
                       .dump();
    sel.roundTrips = dom_->roundTrips(*generated);
    // Drawn exactly as the saved highlight will be: from the CFI, through highlights().
    AnnotationRecord probe;
    probe.position = sel.position;
    for (const auto& box : highlights(page, {probe})) sel.rects.push_back(box.rect);
    if (sel.rects.empty()) return std::nullopt;
    return sel;
}

}  // namespace zk
