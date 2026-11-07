#include "docs/epub/epub_engine.h"

#include <crengine.h>

#include <dirent.h>
#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <mutex>
#include <sstream>

#include "docs/epub/cfi.h"

namespace zk {

namespace {

constexpr double kDpi = 300.0;

std::string utf8(const lString32& s) { return UnicodeToUtf8(s).c_str(); }

// crengine's font manager and hyphenation are process-wide: set them up once.
void initCrengine(const EngineEnv& env) {
    static std::once_flag once;
    std::call_once(once, [&] {
        InitFontManager(lString8::empty_str);
        for (const auto& dir : env.fontDirs) {
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

EpubEngine::EpubEngine(const EngineEnv& env) : env_(env) { initCrengine(env_); }

EpubEngine::~EpubEngine() {
    dom_.reset();  // holds pointers into the view's document
}

void EpubEngine::applyLayout() {
    if (!view_) return;
    view_->setDefaultFontFace(lString8(layout_.font.c_str()));
    view_->setFontSize(static_cast<int>(layout_.sizePt * kDpi / 72.0 + 0.5));
    view_->setPageMargins(lvRect(layout_.marginPx, layout_.marginPx, layout_.marginPx, layout_.marginPx / 2));
    view_->setPageHeaderInfo(0);
    view_->setDefaultInterlineSpace(layout_.lineSpacingPct);
    view_->setViewMode(DVM_PAGES, 1);
    view_->Resize(layout_.width, layout_.height);
}

// crengine settles its position lazily (back to page 0) on the first query after a render.
void EpubEngine::settle() {
    view_->checkRender();
    view_->getCurPage();
    toc_.clear();
}

bool EpubEngine::open(const std::string& path, UserError* err) {
    auto fail = [&](const char* msg, const char* detail) {
        view_.reset();
        if (err) *err = UserError{ErrorKind::Other, msg, detail};
        return false;
    };
    if (!looksLikeZip(path)) return fail("This EPUB is damaged and can't be opened.", "not a zip file");
    if (hasDrm(path)) return fail("This EPUB is protected (DRM) and can't be opened.", "META-INF/encryption.xml");
    view_ = std::make_unique<LVDocView>(8);
    std::ifstream css(env_.dataDir + "/epub.css");
    std::stringstream cssText;
    cssText << css.rdbuf();
    view_->setStyleSheet(lString8(cssText.str().c_str()));
    applyLayout();
    if (!view_->LoadDocument(path.c_str()) || !view_->getDocument())
        return fail("This EPUB is damaged and can't be opened.", "crengine: LoadDocument failed");
    view_->setViewMode(DVM_PAGES, 1);
    settle();
    if (view_->getPageCount() <= 0) return fail("This EPUB is damaged and can't be opened.", "no pages");
    dom_ = std::make_unique<EpubDom>(view_->getDocument(), path);
    return true;
}

void EpubEngine::setLayout(const Layout& l) {
    if (l == layout_ && view_) return;
    layout_ = l;
    applyLayout();
    if (view_) settle();
}

int EpubEngine::pageCount() { return view_ ? view_->getPageCount() : 0; }

PageImage EpubEngine::render(int page) {
    PageImage img;
    if (!view_) return img;
    view_->goToPage(page);
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
    if (!view_) return {};
    ldomXPointer p = view_->getPageBookmark(page);
    return p.isNull() ? DocPosition{} : DocPosition{utf8(p.toString())};
}

int EpubEngine::pageOf(const DocPosition& pos) {
    if (!view_ || pos.value.empty()) return 0;
    ldomXPointer p = view_->getDocument()->createXPointer(Utf8ToUnicode(lString8(pos.value.c_str())));
    if (p.isNull()) return 0;
    const int page = view_->getBookmarkPage(p);
    return page < 0 ? 0 : page;
}

std::vector<TocEntry> EpubEngine::toc() {
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
                            c->getPage()});
            walk(c);
        }
    };
    walk(root);
    return toc_;
}

std::string EpubEngine::chapterAt(int page) {
    std::string title;
    for (const auto& t : toc())
        if (t.page <= page) title = t.title;
    return title;
}

std::vector<SearchHit> EpubEngine::search(const std::string& query, int maxHits) {
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
        h.page = view_->getBookmarkPage(start);
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

int EpubEngine::pageOfAnnotation(const AnnotationRecord& a) {
    if (!view_ || !dom_) return -1;
    auto r = dom_->resolve(cfiOf(a));
    if (!r) return -1;
    return view_->getBookmarkPage(r->start);
}

std::vector<HighlightBox> EpubEngine::highlights(int page, const std::vector<AnnotationRecord>& anns) {
    std::vector<HighlightBox> out;
    if (!view_ || !dom_) return out;
    view_->goToPage(page);
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
    if (!view_ || toPage <= fromPage) return 0;
    ldomXPointer a = view_->getPageBookmark(fromPage), b = view_->getPageBookmark(std::min(toPage, pageCount() - 1));
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

}  // namespace zk
