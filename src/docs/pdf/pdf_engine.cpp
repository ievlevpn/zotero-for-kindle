#include "docs/pdf/pdf_engine.h"

#include "docs/annotation_text.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <nlohmann/json.hpp>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <cstdio>
#include <cmath>
#include <functional>

namespace zk {

PdfEngine::PdfEngine(size_t storeBytes) { newContext(storeBytes); }

void PdfEngine::newContext(size_t storeBytes) {
    if (ctx_) fz_drop_context(ctx_);
    ctx_ = fz_new_context(nullptr, nullptr, storeBytes);
    storeBytes_ = storeBytes;
    if (ctx_) fz_register_document_handlers(ctx_);
}

bool PdfEngine::openForText(const std::string& path, UserError* err) {
    if (!doc_ && storeBytes_ != kTextStoreBytes) newContext(kTextStoreBytes);  // nothing opened in it yet
    return open(path, err);
}

void PdfEngine::trimCaches() {
    if (ctx_) fz_empty_store(ctx_);
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
    if (layout_.columnView) sliceColumns();  // the layout came first
    return true;
}

void PdfEngine::setLayout(const Layout& l) {
    layout_ = l;
    if (l.columnView) sliceColumns();
}

int PdfEngine::pdfPages() {
    if (!doc_) return 0;
    int n = 0;
    fz_try(ctx_) { n = fz_count_pages(ctx_, doc_); }
    fz_catch(ctx_) { n = 0; }
    return n;
}

int PdfEngine::pageCount() { return pdfPages() * viewsPerPage(); }

bool PdfEngine::columnsOn() { return layout_.columnView && columns_.analysed && columns_.bottom > columns_.top; }

int PdfEngine::viewsPerPage() { return columnsOn() ? columns_.columns * slices_.count : 1; }

// ---- Column view ----------------------------------------------------------------------------

namespace {

float median(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

// The most common value of `v` to within 3 pt, and how many are within 4 pt of it.
std::pair<float, int> commonEdge(const std::vector<float>& v) {
    std::map<int, int> bins;
    for (float x : v) ++bins[static_cast<int>(std::lround(x / 3))];
    int best = 0, bestN = -1;
    for (const auto& [b, n] : bins)
        if (n > bestN) best = b, bestN = n;
    const float edge = best * 3.0f;
    const int near = static_cast<int>(std::count_if(v.begin(), v.end(), [&](float x) { return std::abs(x - edge) <= 4; }));
    return {edge, near};
}

}  // namespace

// Once per document: the text area (the median of sample pages' text boxes, so a stamp or a
// running header on one page doesn't widen it) and one or two columns. Two when a fifth or more
// of the lines start at one x right of the middle and a sixth or more end at one x left of it
// (justified columns); single-column maths papers score under 0.1 on both (spike, 2026-11-08).
void PdfEngine::analyseColumns() {
    if (columns_.analysed || !doc_) return;
    columns_.analysed = true;
    const int n = pdfPages();
    std::vector<int> sample;
    if (n <= 5) {
        for (int i = 0; i < n; ++i) sample.push_back(i);
    } else {
        for (double f : {0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8})
            if (sample.empty() || sample.back() != static_cast<int>(n * f)) sample.push_back(static_cast<int>(n * f));
    }
    struct L {
        float x0, y0, x1, y1;
    };
    std::vector<L> all;
    std::vector<float> bx0, by0, bx1, by1;
    for (int page : sample) {
        std::vector<L> lines;
        fz_stext_page* st = nullptr;
        fz_var(st);
        fz_try(ctx_) {
            st = fz_new_stext_page_from_page_number(ctx_, doc_, page, nullptr);
            for (fz_stext_block* b = st->first_block; b; b = b->next) {
                if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
                for (fz_stext_line* l = b->u.t.first_line; l; l = l->next)
                    if (std::abs(l->dir.y) <= 0.1f && l->bbox.x1 - l->bbox.x0 >= 5)
                        lines.push_back({l->bbox.x0, l->bbox.y0, l->bbox.x1, l->bbox.y1});
            }
        }
        fz_always(ctx_) { fz_drop_stext_page(ctx_, st); }
        fz_catch(ctx_) { lines.clear(); }
        if (lines.size() < 8) continue;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const L& l : lines) x0 = std::min(x0, l.x0), y0 = std::min(y0, l.y0), x1 = std::max(x1, l.x1), y1 = std::max(y1, l.y1);
        bx0.push_back(x0), by0.push_back(y0), bx1.push_back(x1), by1.push_back(y1);
        all.insert(all.end(), lines.begin(), lines.end());
    }
    if (bx0.empty()) {  // a scan: no text to measure, so the inked area, one column
        for (int page : sample) {
            float c[4];
            if (!contentBox(page, c)) continue;
            bx0.push_back(c[0]), by0.push_back(c[1]), bx1.push_back(c[2]), by1.push_back(c[3]);
        }
        if (bx0.empty()) return;
    } else {
        columns_.text = true;
    }
    const float x0 = median(bx0), x1 = median(bx1);
    const float y0 = *std::min_element(by0.begin(), by0.end()), y1 = *std::max_element(by1.begin(), by1.end());
    const float w = x1 - x0, mid = (x0 + x1) / 2;
    if (w <= 0 || y1 <= y0) return;
    const float pad = 0.015f * w;  // a little white beside the text
    columns_.x[0] = x0 - pad;
    columns_.x[1] = x1 + pad;
    columns_.top = y0 - pad;
    columns_.bottom = y1 + pad;
    if (!columns_.text || all.empty()) return;
    std::vector<float> starts, ends;
    for (const L& l : all) {
        if (l.x0 > mid - 0.02f * w && l.x0 < x1 - 0.25f * w) starts.push_back(l.x0);
        if (l.x1 < mid + 0.02f * w) ends.push_back(l.x1);
    }
    if (starts.empty() || ends.empty()) return;
    const auto [rightEdge, rightN] = commonEdge(starts);
    const auto [leftEnd, leftN] = commonEdge(ends);
    const double total = static_cast<double>(all.size());
    if (rightN / total > 0.2 && leftN / total > 0.15 && leftEnd < rightEdge) {
        columns_.columns = 2;
        columns_.x[2] = columns_.x[1];
        columns_.x[1] = (leftEnd + rightEdge) / 2;  // the middle of the gutter
    }
}

namespace {
constexpr float kOverlap = 24;  // page space: about two lines, whole in both slices where they meet
}  // namespace

// Per layout: each column fitted to the width, cut into as many screen-high slices as the
// document's text needs, overlapping by about two lines.
void PdfEngine::sliceColumns() {
    analyseColumns();
    if (!columnsOn()) return;
    if (slices_.width == layout_.width && slices_.screenHeight == layout_.height) return;
    slices_ = {};
    slices_.width = layout_.width;
    slices_.screenHeight = layout_.height;
    pageViews_.clear();
    const float columnH = columns_.bottom - columns_.top;
    int count = 1;
    for (int c = 0; c < columns_.columns; ++c) {
        const float sliceH = (columns_.x[c + 1] - columns_.x[c]) * layout_.height / layout_.width;
        if (columnH > sliceH && sliceH > 2 * kOverlap)
            count = std::max(count, static_cast<int>(std::ceil((columnH - kOverlap) / (sliceH - kOverlap))));
    }
    slices_.count = std::min(count, 12);
}

bool PdfEngine::textBox(int page, float box[4]) {
    if (auto it = textBoxes_.find(page); it != textBoxes_.end()) {
        std::copy(it->second.begin(), it->second.end(), box);
        return it->second[2] > it->second[0];
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    fz_stext_page* st = nullptr;
    fz_var(st);
    fz_try(ctx_) {
        st = fz_new_stext_page_from_page_number(ctx_, doc_, page, nullptr);
        for (fz_stext_block* b = st->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* l = b->u.t.first_line; l; l = l->next)
                if (std::abs(l->dir.y) <= 0.1f && l->bbox.x1 - l->bbox.x0 >= 5) {
                    x0 = std::min(x0, l->bbox.x0), y0 = std::min(y0, l->bbox.y0);
                    x1 = std::max(x1, l->bbox.x1), y1 = std::max(y1, l->bbox.y1);
                }
        }
    }
    fz_always(ctx_) { fz_drop_stext_page(ctx_, st); }
    fz_catch(ctx_) { x1 = x0 - 1; }
    const bool found = x1 > x0 && y1 > y0;
    textBoxes_[page] = found ? std::array<float, 4>{x0, y0, x1, y1} : std::array<float, 4>{0, 0, 0, 0};
    std::copy(textBoxes_[page].begin(), textBoxes_[page].end(), box);
    return found;
}

const PdfEngine::PageViews& PdfEngine::pageViews(int pdfPage) {
    if (auto it = pageViews_.find(pdfPage); it != pageViews_.end()) return it->second;
    PageViews v;
    const float docX0 = columns_.x[0], docX1 = columns_.x[columns_.columns];
    const float docW = docX1 - docX0;
    float x0 = docX0, x1 = docX1, top = columns_.top, bottom = columns_.bottom, shift = 0;
    float b[4];
    if ((columns_.text && textBox(pdfPage, b)) || (!columns_.text && contentBox(pdfPage, b))) {
        const float pad = 0.015f * docW;
        b[0] -= pad, b[1] -= pad, b[2] += pad, b[3] += pad;
        // A page as wide as the document's text but elsewhere: mirrored margins, the columns move
        // with it. Otherwise (a short page, a centred title) the document's place.
        if (std::abs((b[2] - b[0]) - docW) < 0.1f * docW) shift = b[0] - docX0;
        x0 = std::min(docX0 + shift, b[0]);
        x1 = std::max(docX1 + shift, b[2]);
        top = std::min(top, b[1]);
        bottom = std::max(bottom, b[3]);
    }
    v.x[0] = x0;
    v.x[columns_.columns] = x1;
    if (columns_.columns == 2) v.x[1] = columns_.x[1] + shift;
    v.top = top;
    const float pageH = bottom - top;
    const int n = slices_.count;
    for (int c = 0; c < columns_.columns; ++c) {
        float w = v.x[c + 1] - v.x[c];
        float h = w * layout_.height / layout_.width;
        // Too tall for n slices with their overlap: wider (smaller) slices, never a gap.
        const float need = n > 1 ? (pageH + (n - 1) * kOverlap) / n : pageH;
        if (h < need) {
            h = need;
            w = h * layout_.width / layout_.height;
        }
        v.width[c] = w;
        v.height[c] = h;
        v.step[c] = n > 1 ? std::max(0.0f, pageH - h) / (n - 1) : 0;
    }
    return pageViews_[pdfPage] = v;
}

PdfEngine::Area PdfEngine::viewRect(int page) {
    const PageViews& pv = pageViews(src(page));
    const int view = page % viewsPerPage();
    const int c = view / slices_.count, s = view % slices_.count;
    const float cx = (pv.x[c] + pv.x[c + 1]) / 2, y = pv.top + s * pv.step[c];
    return {cx - pv.width[c] / 2, y, cx + pv.width[c] / 2, y + pv.height[c]};
}

int PdfEngine::viewAt(int pdfPage, float x, float y) {
    const int views = viewsPerPage();
    if (views == 1) return pdfPage;
    const PageViews& pv = pageViews(pdfPage);
    const int c = columns_.columns == 2 && x >= pv.x[1] ? 1 : 0;
    // The first slice that shows the point with a line's room below it.
    int s = 0;
    while (s + 1 < slices_.count && y > pv.top + s * pv.step[c] + pv.height[c] - 12) ++s;
    return pdfPage * views + c * slices_.count + s;
}

// ---- Fitting a page (or a view) into the page area ------------------------------------------

bool PdfEngine::fit(int page, double& scale, double& dx, double& dy) {
    if (!doc_) return false;
    if (columnsOn()) {  // the view's area, its width across the page area
        const Area v = viewRect(page);
        scale = layout_.width / static_cast<double>(v.x1 - v.x0);
        dx = -v.x0 * scale;
        dy = -v.y0 * scale;
        return true;
    }
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
    float c[4];
    if (layout_.trimMargins && contentBox(page, c)) {
        // The inked area plus a little white around it (2% of the page), never beyond the page.
        const float pad = 0.02f * std::max(r.x1 - r.x0, r.y1 - r.y0);
        r = fz_intersect_rect(r, fz_make_rect(c[0] - pad, c[1] - pad, c[2] + pad, c[3] + pad));
    }
    const double w = r.x1 - r.x0, h = r.y1 - r.y0;
    scale = std::min(layout_.width / w, layout_.height / h);
    dx = (layout_.width - w * scale) / 2 - r.x0 * scale;
    dy = (layout_.height - h * scale) / 2 - r.y0 * scale;
    return true;
}

bool PdfEngine::contentBox(int page, float box[4]) {
    if (auto it = contentBoxes_.find(page); it != contentBoxes_.end()) {
        std::copy(it->second.begin(), it->second.end(), box);
        return it->second[2] > it->second[0];
    }
    fz_rect bbox = fz_empty_rect;
    fz_page* p = nullptr;
    fz_device* dev = nullptr;
    fz_var(p);
    fz_var(dev);
    bool ok = true;
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, page);
        dev = fz_new_bbox_device(ctx_, &bbox);
        fz_run_page(ctx_, p, dev, fz_identity, nullptr);
        fz_close_device(ctx_, dev);
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, dev);
        fz_drop_page(ctx_, p);
    }
    fz_catch(ctx_) { ok = false; }
    const bool found = ok && !fz_is_empty_rect(bbox) && bbox.x1 > bbox.x0 && bbox.y1 > bbox.y0;
    const std::array<float, 4> v = found ? std::array<float, 4>{bbox.x0, bbox.y0, bbox.x1, bbox.y1}
                                         : std::array<float, 4>{0, 0, 0, 0};
    contentBoxes_[page] = v;
    std::copy(v.begin(), v.end(), box);
    return found;
}

PageImage PdfEngine::render(int page) {
    PageImage img;
    img.width = layout_.width;
    img.height = layout_.height;
    img.gray.assign(static_cast<size_t>(img.width) * img.height, 255);
    double scale, dx, dy;
    if (!fit(page, scale, dx, dy)) return img;
    // Drawn into a pixmap the page area's size: a view (column view) draws only what it shows,
    // not its whole page at the view's zoom.
    fz_pixmap* pix = nullptr;
    fz_page* pg = nullptr;
    fz_device* dev = nullptr;
    fz_var(pix);
    fz_var(pg);
    fz_var(dev);
    bool ok = true;
    fz_try(ctx_) {
        const fz_matrix ctm = fz_concat(fz_scale(static_cast<float>(scale), static_cast<float>(scale)),
                                        fz_translate(static_cast<float>(dx), static_cast<float>(dy)));
        pg = fz_load_page(ctx_, doc_, src(page));
        pix = fz_new_pixmap_with_bbox(ctx_, fz_device_gray(ctx_), fz_make_irect(0, 0, img.width, img.height), nullptr, 0);
        fz_clear_pixmap_with_value(ctx_, pix, 255);
        dev = fz_new_draw_device(ctx_, fz_identity, pix);
        fz_run_page(ctx_, pg, dev, ctm, nullptr);
        fz_close_device(ctx_, dev);
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, dev);
        fz_drop_page(ctx_, pg);
    }
    fz_catch(ctx_) { ok = false; }
    if (!ok) {
        fz_drop_pixmap(ctx_, pix);
        return img;
    }
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

// Positions are PDF pages (a view's is its page's); a page opens at its first view.
DocPosition PdfEngine::positionOfPage(int page) { return {"p:" + std::to_string(src(page))}; }

int PdfEngine::pageOf(const DocPosition& pos) {
    if (pos.value.rfind("p:", 0) != 0) return 0;
    try {
        return std::clamp(std::stoi(pos.value.substr(2)), 0, std::max(0, pdfPages() - 1)) * viewsPerPage();
    } catch (...) {
        return 0;
    }
}

std::vector<TocEntry> PdfEngine::tocInViews() {
    std::vector<TocEntry> out = toc_;  // toc_ counts PDF pages: each to the view with its heading
    for (size_t i = 0; i < out.size(); ++i) {
        const auto [x, y] = tocPlaces_[i];
        out[i].page = std::isnan(y) ? out[i].page * viewsPerPage() : viewAt(out[i].page, std::isnan(x) ? 0 : x, y);
    }
    return out;
}

std::vector<TocEntry> PdfEngine::toc() {
    if (tocLoaded_ || !doc_) return tocInViews();
    tocLoaded_ = true;
    fz_outline* outline = nullptr;
    fz_try(ctx_) { outline = fz_load_outline(ctx_, doc_); }
    fz_catch(ctx_) { return toc_; }
    std::function<void(fz_outline*, int)> walk = [&](fz_outline* o, int level) {
        for (; o; o = o->next) {
            int page = -1;
            fz_try(ctx_) { page = fz_page_number_from_location(ctx_, doc_, o->page); }
            fz_catch(ctx_) { page = -1; }
            if (o->title && page >= 0) {
                toc_.push_back({o->title, level, {"p:" + std::to_string(page)}, page});
                tocPlaces_.push_back({o->x, o->y});
            }
            walk(o->down, level + 1);
        }
    };
    walk(outline, 0);
    fz_drop_outline(ctx_, outline);
    return tocInViews();
}

std::string PdfEngine::chapterAt(int page) {
    std::string title;
    for (const auto& t : toc())
        if (t.page <= page) title = t.title;
    return title;
}

std::string PdfEngine::pageText(int page, bool* failed) {
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
        if (failed) *failed = true;
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
    const int pages = pdfPages();
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
            const fz_rect r = fz_rect_from_quad(quads[k < 16 ? k : 15]);
            out.push_back({viewAt(p, (r.x0 + r.x1) / 2, r.y0), snippet, {"p:" + std::to_string(p)}});
        }
    }
    return out;
}

namespace {

struct PdfPosition {
    int pageIndex = -1;
    std::vector<std::array<double, 4>> rects;
    double fontSize = 0;                     // text annotations
    double width = 0;                        // ink: pen width
    std::vector<std::vector<double>> paths;  // ink: x, y, x, y, …
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
    if (j.contains("fontSize") && j["fontSize"].is_number()) out.fontSize = j["fontSize"].get<double>();
    if (j.contains("width") && j["width"].is_number()) out.width = j["width"].get<double>();
    if (j.contains("paths") && j["paths"].is_array())
        for (const auto& path : j["paths"]) {
            if (!path.is_array()) continue;
            std::vector<double> xy;
            for (const auto& v : path)
                if (v.is_number()) xy.push_back(v.get<double>());
            if (xy.size() >= 2) out.paths.push_back(std::move(xy));
        }
    return out;
}

}  // namespace

std::vector<LinkBox> PdfEngine::links(int page) {
    std::vector<LinkBox> out;
    double scale = 1, dx = 0, dy = 0;
    if (page < 0 || page >= pageCount() || !fit(page, scale, dx, dy)) return out;
    const Rect frame{0, 0, layout_.width, layout_.height};
    fz_page* p = nullptr;
    fz_link* links = nullptr;
    fz_var(p);
    fz_var(links);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, src(page));
        links = fz_load_links(ctx_, p);
        int i = 0;
        for (fz_link* l = links; l; l = l->next, ++i) {
            const Rect r{static_cast<int>(l->rect.x0 * scale + dx), static_cast<int>(l->rect.y0 * scale + dy),
                         static_cast<int>((l->rect.x1 - l->rect.x0) * scale), static_cast<int>((l->rect.y1 - l->rect.y0) * scale)};
            const Rect shown = r.intersected(frame);  // a view shows part of the page
            if (!shown.empty() && l->uri && *l->uri) out.push_back({shown, i});
        }
    }
    fz_always(ctx_) {
        fz_drop_link(ctx_, links);
        fz_drop_page(ctx_, p);
    }
    fz_catch(ctx_) { return {}; }
    return out;
}

std::optional<LinkInfo> PdfEngine::link(int page, int index) {
    if (page < 0 || page >= pageCount() || index < 0) return std::nullopt;
    std::string uri;
    fz_page* p = nullptr;
    fz_link* links = nullptr;
    fz_var(p);
    fz_var(links);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, src(page));
        links = fz_load_links(ctx_, p);
        int i = 0;
        for (fz_link* l = links; l; l = l->next, ++i)
            if (i == index && l->uri) uri = l->uri;
    }
    fz_always(ctx_) {
        fz_drop_link(ctx_, links);
        fz_drop_page(ctx_, p);
    }
    fz_catch(ctx_) { return std::nullopt; }
    if (uri.empty()) return std::nullopt;
    LinkInfo info;
    if (fz_is_external_link(ctx_, uri.c_str())) {
        info.kind = LinkInfo::Kind::Web;
        info.label = "Web link";
        info.url = uri.rfind("doi:", 0) == 0 ? "https://doi.org/" + uri.substr(4) : uri;
        return info;
    }
    float x = 0, y = 0;
    int target = -1;
    fz_try(ctx_) { target = fz_page_number_from_location(ctx_, doc_, fz_resolve_link(ctx_, doc_, uri.c_str(), &x, &y)); }
    fz_catch(ctx_) { target = -1; }
    if (target < 0 || target >= pdfPages()) return std::nullopt;
    info.kind = LinkInfo::Kind::Reference;
    // The view with the target; a height alone (FitH) picks the slice, in the first column.
    info.targetPage = std::isnan(y) ? target * viewsPerPage() : viewAt(target, std::isnan(x) ? 0 : x, y);
    info.target = {};  // the view is what to show (a position would open the page's first)
    info.label = "Page " + std::to_string(target + 1);
    info.text = textAt(target, x, y);
    return info;
}

std::string PdfEngine::textAt(int page, float x, float y) {
    struct Line {
        float x0, y0, x1, y1;
        std::string text;
        float mid() const { return (y0 + y1) / 2; }
        float height() const { return y1 - y0; }
    };
    std::vector<Line> lines;
    float pageHeight = 0;
    fz_stext_page* st = nullptr;
    fz_var(st);
    fz_try(ctx_) {
        st = fz_new_stext_page_from_page_number(ctx_, doc_, page, nullptr);
        pageHeight = st->mediabox.y1 - st->mediabox.y0;
        for (fz_stext_block* b = st->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
                if (std::abs(l->dir.y) > 0.1f || l->dir.x < 0.9f) continue;  // margin stamps run sideways
                Line line{l->bbox.x0, l->bbox.y0, l->bbox.x1, l->bbox.y1, {}};
                for (fz_stext_char* c = l->first_char; c; c = c->next) {
                    char buf[8];
                    line.text.append(buf, static_cast<size_t>(fz_runetochar(buf, c->c)));
                }
                const size_t lead = line.text.find_first_not_of(' ');
                if (lead == std::string::npos) continue;
                line.text = line.text.substr(lead);
                lines.push_back(std::move(line));
            }
        }
    }
    fz_always(ctx_) { fz_drop_stext_page(ctx_, st); }
    fz_catch(ctx_) { return ""; }
    const bool noX = std::isnan(x), noY = std::isnan(y);
    if (noY) y = pageHeight * 0.06f;  // the page's top, below a running header
    // The first line whose middle is below the target (hyperref puts it at the previous line's
    // baseline), in the target's column.
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.y0 < b.y0 || (a.y0 == b.y0 && a.x0 < b.x0); });
    const Line* first = nullptr;
    for (const Line& l : lines)
        if (l.mid() > y && (noX || (l.x1 > x - 4 && l.x0 < x + 60))) {
            first = &l;
            break;
        }
    if (!first) return "";
    // The column: lines starting near the first line's left edge or indented from it.
    const float colX0 = first->x0, colX1 = std::max(first->x1, colX0 + 40);
    std::vector<const Line*> column;
    for (const Line& l : lines)
        if (l.y0 >= first->y0 && l.x0 >= colX0 - 6 && l.x0 < colX1) column.push_back(&l);
    // Hanging indents: some continuation in the column (anywhere on the page) is indented, so a
    // line back at the edge starts an entry.
    bool hanging = false;
    for (const Line& l : lines) hanging |= l.x0 > colX0 + 5 && l.x0 < colX1;
    static const std::regex entryStart(R"(^(\[[^\]]{1,12}\]|[0-9]{1,3}\.\s))");
    std::string out;
    const Line* prev = nullptr;
    for (const Line* l : column) {
        if (prev) {
            const bool gap = l->y0 - prev->y1 > prev->height() * 0.8f;
            const bool atEdge = hanging && l->x0 <= colX0 + 1.5f;
            if (gap || atEdge || std::regex_search(l->text, entryStart) || out.size() > 700) break;
            if (!out.empty() && out.back() == '-') out.pop_back();  // a word broken across lines
            else out += ' ';
        }
        out += l->text;
        prev = l;
    }
    return out;
}

int PdfEngine::pageOfPlaced(const DocPosition& pos) {
    if (pos.value.rfind("p:", 0) != 0) return -1;
    try {
        const int page = std::stoi(pos.value.substr(2));
        return page >= 0 && page < pdfPages() ? page * viewsPerPage() : -1;
    } catch (...) {
        return -1;
    }
}

std::string PdfEngine::zoteroPosition(int page) {
    if (page < 0 || page >= pageCount()) return "";
    return std::to_string(src(page));  // Zotero's lastPageIndex for a PDF: the page index
}

int PdfEngine::pageOfZoteroPosition(const std::string& v) {
    const auto j = nlohmann::json::parse(v, nullptr, false);
    if (!j.is_number_integer()) return -1;
    const int page = j.get<int>();
    return page >= 0 && page < pdfPages() ? page * viewsPerPage() : -1;
}

int PdfEngine::pageOfAnnotation(const AnnotationRecord& a) {
    const PdfPosition p = parsePosition(a.position);
    if (p.pageIndex < 0 || p.pageIndex >= pdfPages()) return -1;
    if (viewsPerPage() == 1) return p.pageIndex;
    // The view with its first rect (or its first stroke point), in page space.
    double ux = 0, uy = 0;
    if (!p.rects.empty()) ux = (p.rects[0][0] + p.rects[0][2]) / 2, uy = std::max(p.rects[0][1], p.rects[0][3]);
    else if (!p.paths.empty()) ux = p.paths[0][0], uy = p.paths[0][1];
    else return p.pageIndex * viewsPerPage();
    fz_matrix userToPage = fz_identity;
    fz_page* pg = nullptr;
    fz_var(pg);
    fz_try(ctx_) {
        pg = fz_load_page(ctx_, doc_, p.pageIndex);
        if (pdf_page* pp = pdf_page_from_fz_page(ctx_, pg)) {
            fz_rect media;
            pdf_page_transform(ctx_, pp, &media, &userToPage);
        }
    }
    fz_always(ctx_) { fz_drop_page(ctx_, pg); }
    fz_catch(ctx_) { return p.pageIndex * viewsPerPage(); }
    const fz_point q = fz_transform_point(fz_make_point(static_cast<float>(ux), static_cast<float>(uy)), userToPage);
    return viewAt(p.pageIndex, q.x, q.y);
}

std::vector<HighlightBox> PdfEngine::highlights(int page, const std::vector<AnnotationRecord>& anns) {
    std::vector<HighlightBox> out;
    double scale, dx, dy;
    if (!fit(page, scale, dx, dy)) return out;
    const int pdfPage = src(page);
    // PDF user space (origin bottom-left) -> MuPDF page space (origin top-left), per the page.
    fz_matrix userToPage = fz_identity;
    fz_page* p = nullptr;
    fz_var(p);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, pdfPage);
        if (pdf_page* pp = pdf_page_from_fz_page(ctx_, p)) {
            fz_rect media;
            pdf_page_transform(ctx_, pp, &media, &userToPage);
        }
    }
    fz_always(ctx_) { fz_drop_page(ctx_, p); }
    fz_catch(ctx_) { return out; }
    const Rect frame{0, 0, layout_.width, layout_.height};
    auto toDevice = [&](double x, double y) {
        const fz_point q = fz_transform_point(fz_make_point(static_cast<float>(x), static_cast<float>(y)), userToPage);
        return Point{static_cast<int>(std::lround(q.x * scale + dx)), static_cast<int>(std::lround(q.y * scale + dy))};
    };
    for (const auto& a : anns) {
        const PdfPosition pos = parsePosition(a.position);
        if (pos.pageIndex != pdfPage) continue;
        if (a.type == "ink") {  // strokes, boxed by their extent
            HighlightBox b{a.key, {}, shadeOf(a.color), false};
            b.kind = HighlightBox::Kind::Ink;
            b.strokePx = std::max(3, static_cast<int>(std::lround(pos.width * scale)));
            Rect extent;
            for (const auto& xy : pos.paths) {
                std::vector<Point> path;
                for (size_t i = 0; i + 1 < xy.size(); i += 2) {
                    path.push_back(toDevice(xy[i], xy[i + 1]));
                    extent = extent.united({path.back().x - b.strokePx, path.back().y - b.strokePx, 2 * b.strokePx + 1,
                                            2 * b.strokePx + 1});
                }
                b.paths.push_back(std::move(path));
            }
            if (!b.paths.empty() && !extent.intersected(frame).empty()) {
                b.rect = extent;
                out.push_back(std::move(b));
            }
            continue;
        }
        for (const auto& r : pos.rects) {
            fz_rect q = fz_transform_rect(fz_make_rect(static_cast<float>(r[0]), static_cast<float>(r[1]),
                                                       static_cast<float>(r[2]), static_cast<float>(r[3])),
                                          userToPage);
            const int x0 = static_cast<int>(std::floor(q.x0 * scale + dx)), y0 = static_cast<int>(std::floor(q.y0 * scale + dy));
            const int x1 = static_cast<int>(std::ceil(q.x1 * scale + dx)), y1 = static_cast<int>(std::ceil(q.y1 * scale + dy));
            if (x1 <= x0 || y1 <= y0) continue;
            if (Rect{x0, y0, x1 - x0, y1 - y0}.intersected(frame).empty()) continue;  // on another view
            HighlightBox b{a.key, {x0, y0, x1 - x0, y1 - y0}, shadeOf(a.color), a.type == "underline"};
            if (a.type == "text") {
                b.kind = HighlightBox::Kind::Text;
                b.text = a.comment;
                b.fontPx = static_cast<int>(std::lround((pos.fontSize > 0 ? pos.fontSize : 12) * scale));
            } else if (a.type == "note") {
                b.kind = HighlightBox::Kind::Note;
            } else if (a.type == "image") {
                b.kind = HighlightBox::Kind::Image;
            }
            const bool single = b.kind != HighlightBox::Kind::Highlight;  // one box for these
            out.push_back(std::move(b));
            if (single) break;
        }
    }
    return out;
}

int64_t PdfEngine::wordsBetween(int fromPage, int toPage) {
    // A view counts as its share of its page's words (column view); a page as its words.
    const int views = viewsPerPage();
    auto pageWords = [&](int p) {
        if (auto it = pageWords_.find(p); it != pageWords_.end()) return it->second;
        int64_t words = 0;
        bool inWord = false;
        for (char c : pageText(p)) {
            const bool space = c == ' ' || c == '\n' || c == '\t';
            if (!space && !inWord) ++words;
            inWord = !space;
        }
        return pageWords_[p] = words;
    };
    int64_t total = 0;  // in view-shares, divided at the end so they add up to whole pages
    for (int v = std::max(0, fromPage); v < std::min(toPage, pageCount()); ++v) total += pageWords(v / views);
    return total / views;
}

// Pages in order, "\f" between them. This engine has its own MuPDF context, so nothing is shared
// with the reader's engine.
bool PdfEngine::fullText(std::string& out, UserError* err) {
    out.clear();
    if (!doc_) {
        if (err) *err = UserError{ErrorKind::Other, "This PDF's text can't be read.", "no document"};
        return false;
    }
    const int pages = pdfPages();
    for (int p = 0; p < pages && out.size() <= kMaxFullTextBytes; ++p) {
        if (p) out += '\f';
        bool failed = false;
        out += pageText(p, &failed);
        if (failed) {
            out.clear();
            if (err) *err = UserError{ErrorKind::Other, "This PDF's text can't be read.", "MuPDF failed on page " + std::to_string(p + 1)};
            return false;
        }
    }
    return true;
}

namespace {

template <class F> void forEachChar(fz_stext_page* st, F&& f) {
    for (fz_stext_block* blk = st->first_block; blk; blk = blk->next) {
        if (blk->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* ln = blk->u.t.first_line; ln; ln = ln->next)
            for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) f(ln, ch);
    }
}

// Reading-order index of the character under `p`, else of the nearest one.
int charIndexAt(fz_stext_page* st, fz_point p) {
    int index = 0, best = -1, hit = -1;
    float bestDistance = 1e30f;
    forEachChar(st, [&](fz_stext_line*, fz_stext_char* ch) {
        const fz_rect r = fz_rect_from_quad(ch->quad);
        if (hit < 0 && fz_is_point_inside_rect(p, r)) hit = index;
        const float cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
        const float d = (cx - p.x) * (cx - p.x) + (cy - p.y) * (cy - p.y);
        if (d < bestDistance) {
            bestDistance = d;
            best = index;
        }
        ++index;
    });
    return hit >= 0 ? hit : best;
}

// A PDF's line breaks inside a selection are not paragraph breaks: lines are joined with a
// space, and a word hyphenated at the line end is joined back.
std::string joinLines(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\n') {
            out += text[i];
            continue;
        }
        const bool hyphen = !out.empty() && out.back() == '-' && i + 1 < text.size() &&
                            std::islower(static_cast<unsigned char>(text[i + 1]));
        if (hyphen) out.pop_back();
        else out += ' ';
    }
    return out;
}

double round2(double v) { return std::round(v * 100) / 100; }

}  // namespace

std::optional<Selection> PdfEngine::select(int page, Point from, Point to) {
    double scale, dx, dy;
    if (!doc_ || !fit(page, scale, dx, dy)) return std::nullopt;
    fz_point a{static_cast<float>((from.x - dx) / scale), static_cast<float>((from.y - dy) / scale)};
    fz_point b{static_cast<float>((to.x - dx) / scale), static_cast<float>((to.y - dy) / scale)};

    fz_page* p = nullptr;
    fz_stext_page* st = nullptr;
    fz_var(p);
    fz_var(st);
    fz_matrix userToPage = fz_identity;
    fz_rect media = fz_empty_rect;
    char label[64] = "";
    bool ok = true;
    const int pdfPage = src(page);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, pdfPage);
        if (pdf_page* pp = pdf_page_from_fz_page(ctx_, p)) pdf_page_transform(ctx_, pp, &media, &userToPage);
        st = fz_new_stext_page_from_page(ctx_, p, nullptr);
        fz_page_label(ctx_, p, label, sizeof label);
    }
    fz_catch(ctx_) { ok = false; }
    auto cleanup = [&] {
        fz_drop_stext_page(ctx_, st);
        fz_drop_page(ctx_, p);
    };
    if (!ok) {
        cleanup();
        return std::nullopt;
    }

    int chars = 0;
    bool onText = false;
    const float slack = static_cast<float>(12 / scale);
    forEachChar(st, [&](fz_stext_line* ln, fz_stext_char*) {
        ++chars;
        const fz_rect r = ln->bbox;
        onText = onText || (a.x >= r.x0 - slack && a.x <= r.x1 + slack && a.y >= r.y0 - slack && a.y <= r.y1 + slack);
    });
    if (chars == 0) {  // a scan: no text layer to select from
        cleanup();
        return Selection{};
    }
    if (!onText) {
        cleanup();
        return std::nullopt;
    }
    if (charIndexAt(st, b) < charIndexAt(st, a)) std::swap(a, b);

    fz_quad quads[256];
    int nq = 0;
    char* copied = nullptr;
    fz_var(copied);
    fz_try(ctx_) {
        fz_snap_selection(ctx_, st, &a, &b, FZ_SELECT_WORDS);
        nq = fz_highlight_selection(ctx_, st, a, b, quads, 256);
        copied = fz_copy_selection(ctx_, st, a, b, 0);
    }
    fz_catch(ctx_) { ok = false; }
    // The selection's first character, for the sort index's offset.
    int startIndex = 0;
    if (ok && nq > 0) {
        const fz_rect first = fz_rect_from_quad(quads[0]);
        int index = 0;
        startIndex = -1;
        forEachChar(st, [&](fz_stext_line*, fz_stext_char* ch) {
            const fz_rect r = fz_rect_from_quad(ch->quad);
            if (startIndex < 0 && fz_is_point_inside_rect(fz_make_point((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2), first))
                startIndex = index;
            ++index;
        });
        startIndex = std::max(startIndex, 0);
    }
    const std::string raw = ok && copied ? copied : "";
    fz_free(ctx_, copied);
    cleanup();
    if (!ok || nq == 0) return std::nullopt;

    Selection sel;
    sel.text = normaliseAnnotationText(joinLines(raw));
    if (sel.text.empty()) return std::nullopt;
    const fz_matrix pageToUser = fz_invert_matrix(userToPage);
    nlohmann::json rects = nlohmann::json::array();
    double firstTop = 0;
    for (int i = 0; i < nq; ++i) {
        const fz_rect u = fz_transform_rect(fz_rect_from_quad(quads[i]), pageToUser);
        rects.push_back({round2(u.x0), round2(u.y0), round2(u.x1), round2(u.y1)});
        if (i == 0) firstTop = media.y1 - u.y1;
    }
    sel.position = nlohmann::json{{"pageIndex", pdfPage}, {"rects", rects}}.dump();
    char sort[40];
    std::snprintf(sort, sizeof sort, "%05d|%06d|%05d", pdfPage, startIndex,
                  static_cast<int>(std::max(0.0, std::floor(firstTop))));
    sel.sortIndex = sort;
    sel.pageLabel = label[0] ? label : std::to_string(pdfPage + 1);
    // Drawn exactly as the saved highlight will be.
    AnnotationRecord probe;
    probe.position = sel.position;
    for (const auto& box : highlights(page, {probe})) sel.rects.push_back(box.rect);
    return sel;
}

// The inverse of the ink branch of highlights(): device pixels -> MuPDF page space -> PDF user space.
std::optional<InkPlacement> PdfEngine::inkPosition(int page, const std::vector<std::vector<Point>>& paths, int widthPx) {
    double scale, dx, dy;
    if (!doc_ || paths.empty() || !fit(page, scale, dx, dy)) return std::nullopt;
    fz_page* p = nullptr;
    fz_var(p);
    fz_matrix userToPage = fz_identity;
    fz_rect media = fz_empty_rect;
    char label[64] = "";
    bool ok = true;
    const int pdfPage = src(page);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, pdfPage);
        if (pdf_page* pp = pdf_page_from_fz_page(ctx_, p)) pdf_page_transform(ctx_, pp, &media, &userToPage);
        fz_page_label(ctx_, p, label, sizeof label);
    }
    fz_always(ctx_) { fz_drop_page(ctx_, p); }
    fz_catch(ctx_) { ok = false; }
    if (!ok) return std::nullopt;
    const fz_matrix pageToUser = fz_invert_matrix(userToPage);
    nlohmann::json jpaths = nlohmann::json::array();
    double top = 1e18;  // the strokes' highest point, from the top of the page (page space)
    for (const auto& path : paths) {
        if (path.empty()) continue;
        nlohmann::json xy = nlohmann::json::array();
        for (const Point& d : path) {
            const fz_point q = fz_make_point(static_cast<float>((d.x - dx) / scale), static_cast<float>((d.y - dy) / scale));
            top = std::min(top, static_cast<double>(q.y));
            const fz_point u = fz_transform_point(q, pageToUser);
            xy.push_back(round2(u.x));
            xy.push_back(round2(u.y));
        }
        jpaths.push_back(std::move(xy));
    }
    if (jpaths.empty()) return std::nullopt;
    InkPlacement out;
    out.position = nlohmann::json{{"pageIndex", pdfPage}, {"width", round2(widthPx / scale)}, {"paths", jpaths}}.dump();
    char sort[40];
    std::snprintf(sort, sizeof sort, "%05d|%06d|%05d", pdfPage, 0, static_cast<int>(std::max(0.0, std::floor(top))));
    out.sortIndex = sort;
    out.pageLabel = label[0] ? label : std::to_string(pdfPage + 1);
    return out;
}

Rect PdfEngine::pageBox(int page) {
    double scale, dx, dy;
    if (!doc_ || !fit(page, scale, dx, dy)) return {};
    fz_rect b = fz_empty_rect;
    fz_page* p = nullptr;
    fz_var(p);
    bool ok = true;
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, src(page));
        b = fz_bound_page(ctx_, p);
    }
    fz_always(ctx_) { fz_drop_page(ctx_, p); }
    fz_catch(ctx_) { ok = false; }
    if (!ok) return {};
    const int x0 = static_cast<int>(std::ceil(b.x0 * scale + dx)), y0 = static_cast<int>(std::ceil(b.y0 * scale + dy));
    const int x1 = static_cast<int>(std::floor(b.x1 * scale + dx)), y1 = static_cast<int>(std::floor(b.y1 * scale + dy));
    return Rect{x0, y0, x1 - x0, y1 - y0}.intersected({0, 0, layout_.width, layout_.height});
}

// A rectangle in device pixels -> PDF user space, as a Zotero image annotation. The sort index is
// a highlight's: the page, the first character inside the area (else how many come above it),
// and the area's top from the top of the page.
std::optional<InkPlacement> PdfEngine::areaPosition(int page, Rect r) {
    double scale, dx, dy;
    if (!doc_ || r.empty() || !fit(page, scale, dx, dy)) return std::nullopt;
    fz_page* p = nullptr;
    fz_stext_page* st = nullptr;
    fz_var(p);
    fz_var(st);
    fz_matrix userToPage = fz_identity;
    fz_rect media = fz_empty_rect;
    fz_rect bound = fz_empty_rect;
    char label[64] = "";
    bool ok = true;
    const int pdfPage = src(page);
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, pdfPage);
        if (pdf_page* pp = pdf_page_from_fz_page(ctx_, p)) pdf_page_transform(ctx_, pp, &media, &userToPage);
        fz_page_label(ctx_, p, label, sizeof label);
        bound = fz_bound_page(ctx_, p);
        st = fz_new_stext_page_from_page(ctx_, p, nullptr);
    }
    fz_catch(ctx_) { ok = false; }
    fz_rect area = fz_make_rect(static_cast<float>((r.x - dx) / scale), static_cast<float>((r.y - dy) / scale),
                                static_cast<float>((r.x + r.w - dx) / scale), static_cast<float>((r.y + r.h - dy) / scale));
    if (ok) area = fz_intersect_rect(area, bound);  // nothing off the page
    if (fz_is_empty_rect(area)) ok = false;
    int offset = -1, above = 0, index = 0;
    if (ok && st)
        forEachChar(st, [&](fz_stext_line*, fz_stext_char* ch) {
            const fz_rect c = fz_rect_from_quad(ch->quad);
            const fz_point mid = fz_make_point((c.x0 + c.x1) / 2, (c.y0 + c.y1) / 2);
            if (offset < 0 && fz_is_point_inside_rect(mid, area)) offset = index;
            above += mid.y < area.y0;
            ++index;
        });
    fz_drop_stext_page(ctx_, st);
    fz_drop_page(ctx_, p);
    if (!ok) return std::nullopt;
    const fz_rect u = fz_transform_rect(area, fz_invert_matrix(userToPage));
    InkPlacement out;
    out.position = nlohmann::json{{"pageIndex", pdfPage},
                                  {"rects", nlohmann::json::array({{round2(u.x0), round2(u.y0), round2(u.x1), round2(u.y1)}})}}
                       .dump();
    char sort[40];
    std::snprintf(sort, sizeof sort, "%05d|%06d|%05d", pdfPage, offset >= 0 ? offset : above,
                  static_cast<int>(std::max(0.0, std::floor(static_cast<double>(media.y1 - u.y1)))));
    out.sortIndex = sort;
    out.pageLabel = label[0] ? label : std::to_string(pdfPage + 1);
    return out;
}

}  // namespace zk
