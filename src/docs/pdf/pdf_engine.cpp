#include "docs/pdf/pdf_engine.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <nlohmann/json.hpp>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace zk {

PdfEngine::PdfEngine() {
    ctx_ = fz_new_context(nullptr, nullptr, 64 << 20);  // 64 MB resource store
    if (ctx_) fz_register_document_handlers(ctx_);
}

PdfEngine::~PdfEngine() {
    if (doc_) fz_drop_document(ctx_, doc_);
    if (ctx_) fz_drop_context(ctx_);
}

bool PdfEngine::open(const std::string& path, UserError* err) {
    auto fail = [&](const char* msg, std::string detail) {
        if (err) *err = UserError{ErrorKind::Other, msg, std::move(detail)};
        return false;
    };
    if (::access(path.c_str(), R_OK) != 0)
        return fail("This book's file is missing. Open it from the library to download it again.", path);
    if (!ctx_) return fail("This PDF can't be opened.", "no MuPDF context");
    fz_document* doc = nullptr;
    fz_try(ctx_) { doc = fz_open_document(ctx_, path.c_str()); }
    fz_catch(ctx_) { return fail("This PDF is damaged and can't be opened.", fz_caught_message(ctx_)); }
    int needsPassword = 0;
    fz_try(ctx_) { needsPassword = fz_needs_password(ctx_, doc); }
    fz_catch(ctx_) { needsPassword = 0; }
    if (needsPassword) {
        fz_drop_document(ctx_, doc);
        return fail("This PDF is password-protected and can't be opened.", "needs password");
    }
    int pages = 0;
    fz_try(ctx_) { pages = fz_count_pages(ctx_, doc); }
    fz_catch(ctx_) { pages = 0; }
    if (pages <= 0) {
        fz_drop_document(ctx_, doc);
        return fail("This PDF is damaged and can't be opened.", "no pages");
    }
    doc_ = doc;
    return true;
}

void PdfEngine::setLayout(const Layout& l) { layout_ = l; }

int PdfEngine::pageCount() {
    if (!doc_) return 0;
    int n = 0;
    fz_try(ctx_) { n = fz_count_pages(ctx_, doc_); }
    fz_catch(ctx_) { n = 0; }
    return n;
}

bool PdfEngine::fit(int page, double& scale, double& dx, double& dy) {
    if (!doc_) return false;
    fz_rect r = fz_empty_rect;
    bool ok = true;
    fz_page* p = nullptr;
    fz_var(p);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, page);
        r = fz_bound_page(ctx_, p);
    }
    fz_always(ctx_) { fz_drop_page(ctx_, p); }
    fz_catch(ctx_) { ok = false; }
    if (!ok || r.x1 <= r.x0 || r.y1 <= r.y0) return false;
    const double w = r.x1 - r.x0, h = r.y1 - r.y0;
    scale = std::min(layout_.width / w, layout_.height / h);
    dx = (layout_.width - w * scale) / 2 - r.x0 * scale;
    dy = (layout_.height - h * scale) / 2 - r.y0 * scale;
    return true;
}

PageImage PdfEngine::render(int page) {
    PageImage img;
    img.width = layout_.width;
    img.height = layout_.height;
    img.gray.assign(static_cast<size_t>(img.width) * img.height, 255);
    double scale, dx, dy;
    if (!fit(page, scale, dx, dy)) return img;
    fz_pixmap* pix = nullptr;
    fz_try(ctx_) {
        const fz_matrix ctm = fz_concat(fz_scale(static_cast<float>(scale), static_cast<float>(scale)),
                                        fz_translate(static_cast<float>(dx), static_cast<float>(dy)));
        pix = fz_new_pixmap_from_page_number(ctx_, doc_, page, ctm, fz_device_gray(ctx_), 0);
    }
    fz_catch(ctx_) { return img; }
    // The pixmap's origin may be offset; copy it into place.
    const int px = fz_pixmap_x(ctx_, pix), py = fz_pixmap_y(ctx_, pix);
    const int pw = fz_pixmap_width(ctx_, pix), ph = fz_pixmap_height(ctx_, pix);
    const int stride = fz_pixmap_stride(ctx_, pix);
    const unsigned char* samples = fz_pixmap_samples(ctx_, pix);
    for (int y = 0; y < ph; ++y) {
        const int ty = py + y;
        if (ty < 0 || ty >= img.height) continue;
        for (int x = 0; x < pw; ++x) {
            const int tx = px + x;
            if (tx < 0 || tx >= img.width) continue;
            img.gray[static_cast<size_t>(ty) * img.width + tx] = samples[y * stride + x];
        }
    }
    fz_drop_pixmap(ctx_, pix);
    return img;
}

DocPosition PdfEngine::positionOfPage(int page) { return {"p:" + std::to_string(page)}; }

int PdfEngine::pageOf(const DocPosition& pos) {
    if (pos.value.rfind("p:", 0) != 0) return 0;
    try {
        return std::clamp(std::stoi(pos.value.substr(2)), 0, std::max(0, pageCount() - 1));
    } catch (...) {
        return 0;
    }
}

std::vector<TocEntry> PdfEngine::toc() {
    if (tocLoaded_ || !doc_) return toc_;
    tocLoaded_ = true;
    fz_outline* outline = nullptr;
    fz_try(ctx_) { outline = fz_load_outline(ctx_, doc_); }
    fz_catch(ctx_) { return toc_; }
    std::function<void(fz_outline*, int)> walk = [&](fz_outline* o, int level) {
        for (; o; o = o->next) {
            int page = -1;
            fz_try(ctx_) { page = fz_page_number_from_location(ctx_, doc_, o->page); }
            fz_catch(ctx_) { page = -1; }
            if (o->title && page >= 0) toc_.push_back({o->title, level, positionOfPage(page), page});
            walk(o->down, level + 1);
        }
    };
    walk(outline, 0);
    fz_drop_outline(ctx_, outline);
    return toc_;
}

std::string PdfEngine::chapterAt(int page) {
    std::string title;
    for (const auto& t : toc())
        if (t.page <= page) title = t.title;
    return title;
}

std::string PdfEngine::pageText(int page) {
    fz_stext_page* st = nullptr;
    fz_buffer* buf = nullptr;
    fz_var(st);
    fz_var(buf);
    fz_try(ctx_) {
        st = fz_new_stext_page_from_page_number(ctx_, doc_, page, nullptr);
        buf = fz_new_buffer_from_stext_page(ctx_, st);
    }
    fz_catch(ctx_) {
        fz_drop_buffer(ctx_, buf);
        fz_drop_stext_page(ctx_, st);
        return {};
    }
    // Copy out after fz_try: a C++ allocation must not throw inside MuPDF's setjmp scope.
    unsigned char* data = nullptr;
    const size_t n = fz_buffer_storage(ctx_, buf, &data);
    std::string text(reinterpret_cast<char*>(data), n);
    fz_drop_buffer(ctx_, buf);
    fz_drop_stext_page(ctx_, st);
    return text;
}

std::vector<SearchHit> PdfEngine::search(const std::string& query, int maxHits) {
    std::vector<SearchHit> out;
    if (!doc_ || query.empty()) return out;
    const int pages = pageCount();
    for (int p = 0; p < pages && static_cast<int>(out.size()) < maxHits; ++p) {
        fz_quad quads[16];
        int found = 0;
        fz_try(ctx_) { found = fz_search_page_number(ctx_, doc_, p, query.c_str(), nullptr, quads, 16); }
        fz_catch(ctx_) { found = 0; }
        if (!found) continue;
        const std::string text = pageText(p);
        size_t at = 0;
        for (int k = 0; k < found && static_cast<int>(out.size()) < maxHits; ++k) {
            // Case-insensitive find of the k-th occurrence in the page text, for a snippet.
            auto it = std::search(text.begin() + static_cast<long>(at), text.end(), query.begin(), query.end(),
                                  [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                              std::tolower(static_cast<unsigned char>(b)); });
            std::string snippet = query;
            if (it != text.end()) {
                const size_t pos = static_cast<size_t>(it - text.begin());
                const size_t from = pos > 40 ? pos - 40 : 0;
                snippet = (from ? "…" : "") + text.substr(from, pos - from + query.size() + 40);
                for (char& c : snippet)
                    if (c == '\n') c = ' ';
                at = pos + query.size();
            }
            out.push_back({p, snippet, positionOfPage(p)});
        }
    }
    return out;
}

namespace {

struct PdfPosition {
    int pageIndex = -1;
    std::vector<std::array<double, 4>> rects;
};

PdfPosition parsePosition(const std::string& s) {
    PdfPosition out;
    auto j = nlohmann::json::parse(s, nullptr, false);
    if (!j.is_object() || !j.contains("pageIndex") || !j["pageIndex"].is_number_integer()) return out;
    out.pageIndex = j["pageIndex"].get<int>();
    if (j.contains("rects") && j["rects"].is_array())
        for (const auto& r : j["rects"])
            if (r.is_array() && r.size() == 4 && r[0].is_number() && r[1].is_number() && r[2].is_number() && r[3].is_number())
                out.rects.push_back({r[0].get<double>(), r[1].get<double>(), r[2].get<double>(), r[3].get<double>()});
    return out;
}

}  // namespace

int PdfEngine::pageOfAnnotation(const AnnotationRecord& a) {
    const PdfPosition p = parsePosition(a.position);
    return p.pageIndex >= 0 && p.pageIndex < pageCount() ? p.pageIndex : -1;
}

std::vector<HighlightBox> PdfEngine::highlights(int page, const std::vector<AnnotationRecord>& anns) {
    std::vector<HighlightBox> out;
    double scale, dx, dy;
    if (!fit(page, scale, dx, dy)) return out;
    // PDF user space (origin bottom-left) -> MuPDF page space (origin top-left), per the page.
    fz_matrix userToPage = fz_identity;
    fz_page* p = nullptr;
    fz_var(p);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, page);
        if (pdf_page* pp = pdf_page_from_fz_page(ctx_, p)) {
            fz_rect media;
            pdf_page_transform(ctx_, pp, &media, &userToPage);
        }
    }
    fz_always(ctx_) { fz_drop_page(ctx_, p); }
    fz_catch(ctx_) { return out; }
    for (const auto& a : anns) {
        const PdfPosition pos = parsePosition(a.position);
        if (pos.pageIndex != page) continue;
        for (const auto& r : pos.rects) {
            fz_rect q = fz_transform_rect(fz_make_rect(static_cast<float>(r[0]), static_cast<float>(r[1]),
                                                       static_cast<float>(r[2]), static_cast<float>(r[3])),
                                          userToPage);
            const int x0 = static_cast<int>(std::floor(q.x0 * scale + dx)), y0 = static_cast<int>(std::floor(q.y0 * scale + dy));
            const int x1 = static_cast<int>(std::ceil(q.x1 * scale + dx)), y1 = static_cast<int>(std::ceil(q.y1 * scale + dy));
            if (x1 <= x0 || y1 <= y0) continue;
            out.push_back({a.key, {x0, y0, x1 - x0, y1 - y0}, shadeOf(a.color), a.type == "underline"});
        }
    }
    return out;
}

int64_t PdfEngine::wordsBetween(int fromPage, int toPage) {
    int64_t words = 0;
    for (int p = std::max(0, fromPage); p < std::min(toPage, pageCount()); ++p) {
        bool inWord = false;
        for (char c : pageText(p)) {
            const bool space = c == ' ' || c == '\n' || c == '\t';
            if (!space && !inWord) ++words;
            inWord = !space;
        }
    }
    return words;
}

}  // namespace zk
