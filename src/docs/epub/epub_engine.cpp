#include "docs/epub/epub_engine.h"

#include <crengine.h>

#include <dirent.h>

#include <fstream>
#include <mutex>
#include <sstream>

namespace zk {

namespace {

constexpr double kDpi = 300.0;

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
        HyphMan::initDictionaries(Utf8ToUnicode(lString8((env.dataDir + "/hyph").c_str())));
        TextLangMan::setHyphenationEnabled(true);
        TextLangMan::setEmbeddedLangsEnabled(true);
    });
}

}  // namespace

EpubEngine::EpubEngine(const EngineEnv& env) : env_(env) { initCrengine(env_); }

EpubEngine::~EpubEngine() = default;

void EpubEngine::applyLayout() {
    if (!view_) return;
    view_->setDefaultFontFace(lString8(layout_.font.c_str()));
    view_->setFontSize(static_cast<int>(layout_.sizePt * kDpi / 72.0 + 0.5));
    view_->setPageMargins(lvRect(layout_.marginPx, layout_.marginPx, layout_.marginPx, layout_.marginPx / 2));
    view_->setPageHeaderInfo(0);
    view_->setViewMode(DVM_PAGES, 1);
    view_->Resize(layout_.width, layout_.height);
}

bool EpubEngine::open(const std::string& path, UserError* err) {
    view_ = std::make_unique<LVDocView>(8);
    std::ifstream css(env_.dataDir + "/epub.css");
    std::stringstream cssText;
    cssText << css.rdbuf();
    view_->setStyleSheet(lString8(cssText.str().c_str()));
    applyLayout();
    if (!view_->LoadDocument(path.c_str())) {
        view_.reset();
        if (err) *err = UserError{ErrorKind::Other, "This EPUB is damaged and can't be opened.", "crengine: LoadDocument failed"};
        return false;
    }
    view_->setViewMode(DVM_PAGES, 1);
    view_->checkRender();
    view_->getCurPage();  // crengine settles its position lazily (back to page 0) on the first query
    path_ = path;
    return true;
}

void EpubEngine::setLayout(const Layout& l) {
    layout_ = l;
    applyLayout();
    if (view_) {
        view_->checkRender();
        view_->getCurPage();
    }
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

// Filled in by the EPUB engine task (positions, TOC, search, highlights).
DocPosition EpubEngine::positionOfPage(int) { return {}; }
int EpubEngine::pageOf(const DocPosition&) { return 0; }
std::vector<TocEntry> EpubEngine::toc() { return {}; }
std::string EpubEngine::chapterAt(int) { return {}; }
std::vector<SearchHit> EpubEngine::search(const std::string&, int) { return {}; }
std::vector<HighlightBox> EpubEngine::highlights(int, const std::vector<AnnotationRecord>&) { return {}; }
int64_t EpubEngine::wordsBetween(int, int) { return 0; }

}  // namespace zk
