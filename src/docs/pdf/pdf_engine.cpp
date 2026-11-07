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
#include <cstdio>
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
    auto toDevice = [&](double x, double y) {
        const fz_point q = fz_transform_point(fz_make_point(static_cast<float>(x), static_cast<float>(y)), userToPage);
        return Point{static_cast<int>(std::lround(q.x * scale + dx)), static_cast<int>(std::lround(q.y * scale + dy))};
    };
    for (const auto& a : anns) {
        const PdfPosition pos = parsePosition(a.position);
        if (pos.pageIndex != page) continue;
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
            if (!b.paths.empty()) {
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
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, page);
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
    sel.position = nlohmann::json{{"pageIndex", page}, {"rects", rects}}.dump();
    char sort[40];
    std::snprintf(sort, sizeof sort, "%05d|%06d|%05d", page, startIndex,
                  static_cast<int>(std::max(0.0, std::floor(firstTop))));
    sel.sortIndex = sort;
    sel.pageLabel = label[0] ? label : std::to_string(page + 1);
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
    fz_try(ctx_) {
        p = fz_load_page(ctx_, doc_, page);
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
    out.position = nlohmann::json{{"pageIndex", page}, {"width", round2(widthPx / scale)}, {"paths", jpaths}}.dump();
    char sort[40];
    std::snprintf(sort, sizeof sort, "%05d|%06d|%05d", page, 0, static_cast<int>(std::max(0.0, std::floor(top))));
    out.sortIndex = sort;
    out.pageLabel = label[0] ? label : std::to_string(page + 1);
    return out;
}

}  // namespace zk
