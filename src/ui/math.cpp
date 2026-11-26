#include "ui/math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pango/pangocairo.h>

#include "core/formula.h"
#include "latex.h"
#include "render.h"
#include "ui/math_cairo.h"
#include "utils/utf.h"

// Only pango <= 1.26 and cairo <= 1.10 APIs: Kindle firmware compatibility.

namespace zk {

namespace {

constexpr double kDpi = 300.0;
constexpr const char* kObjRepl = "\xEF\xBF\xBC";  // U+FFFC OBJECT REPLACEMENT CHARACTER

std::mutex& mathMutex() {
    static std::mutex mu;
    return mu;
}

bool gMathReady = false;
bool gMathTried = false;
std::string gMathResDir;

std::string detectResDir() {
    namespace fs = std::filesystem;
    std::vector<std::string> candidates;
    if (!gMathResDir.empty()) candidates.push_back(gMathResDir);
#ifdef ZK_KINDLE
    candidates.push_back("/mnt/us/zotero/share/microtex");
#else
    candidates.push_back("third_party/microtex/res");
    candidates.push_back("../third_party/microtex/res");
    candidates.push_back("/Users/ievlevpn/Playground/zotero-for-kindle/third_party/microtex/res");
#endif
    std::error_code ec;
    for (const auto& dir : candidates) {
        if (fs::exists(fs::path(dir) / ".clatexmath-res_root", ec) || fs::exists(fs::path(dir) / "fonts" / "base" / "cmmi10.ttf", ec)) {
            return dir;
        }
    }
    return {};
}

bool ensureMathInitLocked() {
    if (gMathTried) return gMathReady;
    gMathTried = true;
    const std::string dir = detectResDir();
    if (dir.empty()) return false;
    try {
        tex::LaTeX::init(dir);
        gMathReady = true;
    } catch (const std::exception&) {
        gMathReady = false;
    }
    return gMathReady;
}

PangoFontMap* mathFontMap() {
    static PangoFontMap* map = pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    return map;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

struct Segment {
    enum class Kind { Plain, InlineMath, DisplayMath };
    Kind kind = Kind::Plain;
    std::string text;
    std::string raw;  // original source for fallback if MicroTeX parse fails
};

// Scans `s` for $$...$$, \[...\], \(...\), and currency-safe $...$.
std::vector<Segment> scanMath(std::string_view s) {
    std::vector<Segment> out;
    std::string plain;
    auto flushPlain = [&] {
        if (!plain.empty()) {
            out.push_back({Segment::Kind::Plain, std::move(plain), {}});
            plain.clear();
        }
    };

    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        // Escaped dollar: \$ -> literal $
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '$') {
            plain += '$';
            i += 2;
            continue;
        }
        // Display math: \[ ... \]
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '[') {
            const size_t close = s.find("\\]", i + 2);
            if (close != std::string_view::npos && close > i + 2) {
                flushPlain();
                out.push_back({Segment::Kind::DisplayMath, std::string(s.substr(i + 2, close - (i + 2))),
                               std::string(s.substr(i, close + 2 - i))});
                i = close + 2;
                continue;
            }
        }
        // Inline math: \( ... \)
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '(') {
            const size_t close = s.find("\\)", i + 2);
            if (close != std::string_view::npos && close > i + 2) {
                flushPlain();
                out.push_back({Segment::Kind::InlineMath, std::string(s.substr(i + 2, close - (i + 2))),
                               std::string(s.substr(i, close + 2 - i))});
                i = close + 2;
                continue;
            }
        }
        // Display math: $$ ... $$
        if (s[i] == '$' && i + 1 < n && s[i + 1] == '$') {
            size_t j = i + 2;
            size_t close = std::string_view::npos;
            while (j + 1 < n) {
                if (s[j] == '\\' && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (s[j] == '$' && s[j + 1] == '$') {
                    close = j;
                    break;
                }
                ++j;
            }
            if (close != std::string_view::npos && close > i + 2) {
                flushPlain();
                out.push_back({Segment::Kind::DisplayMath, std::string(s.substr(i + 2, close - (i + 2))),
                               std::string(s.substr(i, close + 2 - i))});
                i = close + 2;
                continue;
            }
        }
        // Inline math: $ ... $ (currency-safe: opening not followed by space, closing not preceded by space or followed by digit)
        if (s[i] == '$' && i + 1 < n && !isSpace(s[i + 1]) && s[i + 1] != '$') {
            size_t j = i + 1;
            size_t close = std::string_view::npos;
            while (j < n) {
                if (s[j] == '\\' && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (s[j] == '\n' && j + 1 < n && s[j + 1] == '\n') break;  // don't span blank lines
                if (s[j] == '$') {
                    if (!isSpace(s[j - 1]) && (j + 1 >= n || !isDigit(s[j + 1]))) {
                        close = j;
                        break;
                    }
                }
                ++j;
            }
            if (close != std::string_view::npos && close > i + 1) {
                flushPlain();
                out.push_back({Segment::Kind::InlineMath, std::string(s.substr(i + 1, close - (i + 1))),
                               std::string(s.substr(i, close + 1 - i))});
                i = close + 1;
                continue;
            }
        }
        plain += s[i++];
    }
    flushPlain();
    return out;
}

struct MathGlyph {
    std::unique_ptr<tex::TeXRender> render;
    bool display = false;
    double scale = 1.0;
    int drawW = 0;
    int drawH = 0;
    int ascent = 0;
    int descent = 0;
    int boxW = 0;
    int padX = 0;
    int padY = 0;
};

void renderMathShape(cairo_t* cr, PangoAttrShape* attr, gboolean do_path, gpointer) {
    if (do_path || !attr || !attr->data) return;
    const auto* g = static_cast<const MathGlyph*>(attr->data);
    if (!g->render) return;
    double cx = 0.0, cy = 0.0;
    cairo_get_current_point(cr, &cx, &cy);

    // Pango positions the current point at (logical_x, baseline_y).
    const double x = g->display ? cx + std::max(0.0, (g->boxW - g->drawW) * 0.5) : cx + g->padX;
    const double yTop = cy - g->ascent;

    cairo_save(cr);
    cairo_translate(cr, x, yTop);
    if (g->scale != 1.0) cairo_scale(cr, g->scale, g->scale);
    MicroTeXGraphics g2(cr);
    g->render->draw(g2, 0, 0);
    cairo_restore(cr);
    // Restore current point so Pango continues smoothly after the shape.
    cairo_move_to(cr, cx, cy);
}

std::unique_ptr<MathGlyph> compileFormula(const std::string& latex, bool display, const TextStyle& style, int maxWidth) {
    std::lock_guard<std::mutex> lock(mathMutex());
    if (!ensureMathInitLocked()) return nullptr;
    if (latex.empty() || latex.size() > 2048) return nullptr;

    try {
        const std::wstring wsrc = tex::utf82wide(latex.c_str());
        tex::Formula formula(wsrc);
        // Match Pango's point size at 300 DPI; Computer Modern math matches serif x-height well at ~1.02x.
        const float pxSize = static_cast<float>(style.size_pt * kDpi / 72.0 * (display ? 1.08 : 1.02));
        const uint8_t gr = style.gray;
        const tex::color fg = tex::argb(255, gr, gr, gr);
        tex::TeXRenderBuilder builder;
        std::unique_ptr<tex::TeXRender> r(
            builder.setStyle(display ? tex::TexStyle::display : tex::TexStyle::text)
                .setTextSize(pxSize)
                .setForeground(fg)
                .setTrueValues(true)
                .build(formula));
        if (!r || r->getWidth() <= 0 || r->getHeight() <= 0) return nullptr;

        auto glyph = std::make_unique<MathGlyph>();
        glyph->display = display;
        const int padX = display ? 0 : std::max(2, static_cast<int>(std::round(pxSize * 0.08)));
        const int padY = display ? std::max(8, static_cast<int>(std::round(pxSize * 0.25))) : 2;
        const int availW = std::max(24, maxWidth - 2 * padX);
        double scale = 1.0;
        if (r->getWidth() > availW) scale = static_cast<double>(availW) / static_cast<double>(r->getWidth());

        const int rawH = r->getHeight();
        const int rawD = r->getDepth();
        const int rawA = std::max(1, rawH - rawD);

        glyph->scale = scale;
        glyph->drawW = std::max(1, static_cast<int>(std::ceil(r->getWidth() * scale)));
        glyph->drawH = std::max(1, static_cast<int>(std::ceil(rawH * scale)));
        glyph->ascent = std::max(1, static_cast<int>(std::ceil(rawA * scale)));
        glyph->descent = std::max(0, static_cast<int>(std::ceil(rawD * scale)));
        glyph->padX = padX;
        glyph->padY = padY;
        glyph->boxW = display ? maxWidth : (glyph->drawW + 2 * padX);
        glyph->render = std::move(r);
        return glyph;
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // namespace

void initMath(const std::string& resDir) {
    std::lock_guard<std::mutex> lock(mathMutex());
    if (!resDir.empty()) {
        gMathResDir = resDir;
        gMathTried = false;
    }
    ensureMathInitLocked();
}

bool hasMath(std::string_view utf8) {
    if (utf8.find('$') != std::string_view::npos) return true;
    if (utf8.find("\\(") != std::string_view::npos) return true;
    if (utf8.find("\\[") != std::string_view::npos) return true;
    return false;
}

struct KeptMathText::Impl {
    PangoContext* ctx = nullptr;
    PangoLayout* layout = nullptr;
    std::vector<std::unique_ptr<MathGlyph>> glyphs;
    uint8_t gray = 0;

    ~Impl() {
        if (layout) g_object_unref(layout);
        if (ctx) g_object_unref(ctx);
    }
};

KeptMathText::KeptMathText(std::string_view utf8, const TextStyle& style, int width, int max_lines, Align align)
    : impl_(std::make_unique<Impl>()) {
    impl_->gray = style.gray;
    if (utf8.empty()) {
        tops_ = {0};
        return;
    }

    const int w = std::max(1, width);
    const auto segments = scanMath(utf8);

    struct ShapeSpan {
        guint start;
        guint end;
        MathGlyph* glyph;
    };
    std::string combined;
    std::vector<ShapeSpan> shapes;

    for (const auto& seg : segments) {
        if (seg.kind == Segment::Kind::Plain) {
            combined += seg.text;
            continue;
        }
        const bool display = (seg.kind == Segment::Kind::DisplayMath);
        auto glyph = compileFormula(seg.text, display, style, w);
        if (!glyph) {
            combined += seg.raw;
            continue;
        }
        if (display && !combined.empty() && combined.back() != '\n') combined += '\n';
        const guint start = static_cast<guint>(combined.size());
        combined += kObjRepl;
        const guint end = static_cast<guint>(combined.size());
        MathGlyph* ptr = glyph.get();
        impl_->glyphs.push_back(std::move(glyph));
        shapes.push_back({start, end, ptr});
        if (display) combined += '\n';
    }
    // Trim trailing newline added after a terminal display equation.
    while (!combined.empty() && combined.back() == '\n' && utf8.back() != '\n') combined.pop_back();

    formulaCount_ = static_cast<int>(impl_->glyphs.size());

    impl_->ctx = pango_font_map_create_context(mathFontMap());
    pango_cairo_context_set_resolution(impl_->ctx, kDpi);
    pango_cairo_context_set_shape_renderer(impl_->ctx, renderMathShape, nullptr, nullptr);

    impl_->layout = pango_layout_new(impl_->ctx);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, style.family);
    pango_font_description_set_size(desc, static_cast<int>(style.size_pt * PANGO_SCALE));
    pango_font_description_set_weight(desc, static_cast<PangoWeight>(style.weight));
    pango_font_description_set_style(desc, style.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(impl_->layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_width(impl_->layout, w * PANGO_SCALE);
    pango_layout_set_wrap(impl_->layout, PANGO_WRAP_WORD_CHAR);
    if (max_lines > 0) {
        pango_layout_set_height(impl_->layout, -max_lines);
        pango_layout_set_ellipsize(impl_->layout, PANGO_ELLIPSIZE_END);
    }
    switch (align) {
        case Align::Left: pango_layout_set_alignment(impl_->layout, PANGO_ALIGN_LEFT); break;
        case Align::Center: pango_layout_set_alignment(impl_->layout, PANGO_ALIGN_CENTER); break;
        case Align::Right: pango_layout_set_alignment(impl_->layout, PANGO_ALIGN_RIGHT); break;
        case Align::Justify:
            pango_layout_set_alignment(impl_->layout, PANGO_ALIGN_LEFT);
            pango_layout_set_justify(impl_->layout, TRUE);
            break;
    }

    pango_layout_set_text(impl_->layout, combined.data(), static_cast<int>(combined.size()));

    if (!shapes.empty()) {
        PangoAttrList* attrs = pango_attr_list_new();
        for (const auto& sp : shapes) {
            const MathGlyph* g = sp.glyph;
            PangoRectangle logical;
            logical.x = 0;
            logical.y = -(g->ascent + g->padY) * PANGO_SCALE;
            logical.width = g->boxW * PANGO_SCALE;
            logical.height = (g->ascent + g->descent + 2 * g->padY) * PANGO_SCALE;
            PangoAttribute* attr = pango_attr_shape_new_with_data(&logical, &logical, sp.glyph, nullptr, nullptr);
            attr->start_index = sp.start;
            attr->end_index = sp.end;
            pango_attr_list_insert(attrs, attr);
        }
        pango_layout_set_attributes(impl_->layout, attrs);
        pango_attr_list_unref(attrs);
    }

    PangoRectangle extents;
    pango_layout_get_pixel_extents(impl_->layout, nullptr, &extents);
    width_ = extents.width;

    PangoLayoutIter* it = pango_layout_get_iter(impl_->layout);
    int bottom = 0;
    do {
        PangoRectangle logical;
        pango_layout_iter_get_line_extents(it, nullptr, &logical);
        tops_.push_back(PANGO_PIXELS(logical.y));
        bottom = PANGO_PIXELS(logical.y + logical.height);
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
    tops_.push_back(bottom);
}

KeptMathText::~KeptMathText() = default;

void KeptMathText::drawLines(cairo_t* cr, Rect box, int first, int count) const {
    if (!impl_ || !impl_->layout || count <= 0) return;
    PangoLayoutIter* it = pango_layout_get_iter(impl_->layout);
    const double g = impl_->gray / 255.0;
    cairo_save(cr);
    cairo_set_source_rgb(cr, g, g, g);
    int top = 0;
    for (int i = 0; i < first + count; ++i) {
        PangoRectangle logical;
        pango_layout_iter_get_line_extents(it, nullptr, &logical);
        if (i == first) top = logical.y;
        if (i >= first) {
            const int baseline = pango_layout_iter_get_baseline(it);
            cairo_move_to(cr, box.x + logical.x / double(PANGO_SCALE), box.y + (baseline - top) / double(PANGO_SCALE));
            pango_cairo_show_layout_line(cr, pango_layout_iter_get_line_readonly(it));
        }
        if (!pango_layout_iter_next_line(it)) break;
    }
    pango_layout_iter_free(it);
    cairo_restore(cr);
    cairo_new_path(cr);
}

}  // namespace zk
