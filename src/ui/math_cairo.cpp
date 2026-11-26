#include "ui/math_cairo.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#if __has_include(<fontconfig/fcfreetype.h>)
#include <fontconfig/fcfreetype.h>
#endif
#include <pango/pangocairo.h>

#include "utils/utf.h"

// Only pango <= 1.26, cairo <= 1.10, and fontconfig <= 2.8 APIs: Kindle firmware compatibility.

namespace {

constexpr double kPi = 3.14159265358979323846;

PangoFontMap* ftFontMap() {
    static PangoFontMap* map = pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    return map;
}

class FontCairo final : public tex::Font {
public:
    FontCairo(std::string family, int style, float size, cairo_font_face_t* face = nullptr)
        : family_(std::move(family)), style_(style), size_(size), face_(face) {
        if (face_) cairo_font_face_reference(face_);
    }

    FontCairo(const std::string& file, float size) : style_(tex::PLAIN), size_(size) {
        loadFromFile(file);
    }

    ~FontCairo() override {
        if (face_) cairo_font_face_destroy(face_);
    }

    float getSize() const override { return size_; }

    tex::sptr<tex::Font> deriveFont(int style) const override {
        return tex::sptrOf<FontCairo>(family_, style, size_, face_);
    }

    bool operator==(const tex::Font& other) const override {
        const auto& f = static_cast<const FontCairo&>(other);
        return size_ == f.size_ && style_ == f.style_ && family_ == f.family_ && face_ == f.face_;
    }

    bool operator!=(const tex::Font& other) const override { return !(*this == other); }

    const std::string& family() const { return family_; }
    int style() const { return style_; }
    cairo_font_face_t* face() const { return face_; }

private:
    struct CachedFont {
        std::string family;
        cairo_font_face_t* face = nullptr;
    };

    void loadFromFile(const std::string& file) {
        static std::mutex mu;
        static std::map<std::string, CachedFont> cache;
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = cache.find(file); it != cache.end()) {
            family_ = it->second.family;
            face_ = it->second.face;
            if (face_) cairo_font_face_reference(face_);
            return;
        }
        const auto* fpath = reinterpret_cast<const FcChar8*>(file.c_str());
        int count = 0;
        FcBlanks* blanks = FcConfigGetBlanks(nullptr);
        FcPattern* pat = FcFreeTypeQuery(fpath, 0, blanks, &count);
        if (!pat) {
            cache[file] = {};
            return;
        }
        FcChar8* fam = nullptr;
        if (FcPatternGetString(pat, FC_FAMILY, 0, &fam) == FcResultMatch && fam) {
            family_ = reinterpret_cast<const char*>(fam);
        }
        FcConfigAppFontAddFile(nullptr, fpath);
        cairo_font_face_t* created = cairo_ft_font_face_create_for_pattern(pat);
        FcPatternDestroy(pat);
        if (created && cairo_font_face_status(created) == CAIRO_STATUS_SUCCESS) {
            cache[file] = {family_, created};
            face_ = cairo_font_face_reference(created);
        } else {
            if (created) cairo_font_face_destroy(created);
            cache[file] = {family_, nullptr};
        }
    }

    std::string family_;
    int style_ = tex::PLAIN;
    float size_ = 10.f;
    cairo_font_face_t* face_ = nullptr;
};

void applyFontDesc(PangoLayout* layout, const std::string& family, int style, float sizePx) {
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, family.empty() ? "serif" : family.c_str());
    pango_font_description_set_absolute_size(desc, static_cast<double>(sizePx) * PANGO_SCALE);
    pango_font_description_set_weight(desc, (style & tex::BOLD) ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(desc, (style & tex::ITALIC) ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
}

class TextLayoutCairo final : public tex::TextLayout {
public:
    TextLayoutCairo(const std::wstring& src, const tex::sptr<FontCairo>& f) {
        ctx_ = pango_font_map_create_context(ftFontMap());
        layout_ = pango_layout_new(ctx_);
        const std::string utf8 = tex::wide2utf8(src);
        pango_layout_set_text(layout_, utf8.data(), static_cast<int>(utf8.size()));
        applyFontDesc(layout_, f ? f->family() : "serif", f ? f->style() : tex::PLAIN, f ? f->getSize() : 20.f);
        PangoLayoutIter* it = pango_layout_get_iter(layout_);
        ascent_ = static_cast<float>(pango_layout_iter_get_baseline(it)) / static_cast<float>(PANGO_SCALE);
        pango_layout_iter_free(it);
    }

    ~TextLayoutCairo() {
        if (layout_) g_object_unref(layout_);
        if (ctx_) g_object_unref(ctx_);
    }

    void getBounds(tex::Rect& r) override {
        int w = 0, h = 0;
        pango_layout_get_pixel_size(layout_, &w, &h);
        r.x = 0.f;
        r.y = -ascent_;
        r.w = static_cast<float>(w);
        r.h = static_cast<float>(h);
    }

    void draw(tex::Graphics2D& g2, float x, float y) override {
        auto& g = static_cast<zk::MicroTeXGraphics&>(g2);
        cairo_t* cr = g.cr();
        cairo_save(cr);
        cairo_translate(cr, x, y - ascent_);
        pango_cairo_update_layout(cr, layout_);
        pango_cairo_show_layout(cr, layout_);
        cairo_restore(cr);
    }

private:
    PangoContext* ctx_ = nullptr;
    PangoLayout* layout_ = nullptr;
    float ascent_ = 0.f;
};

}  // namespace

namespace tex {

Font* Font::create(const std::string& file, float size) {
    return new FontCairo(file, size);
}

sptr<Font> Font::_create(const std::string& name, int style, float size) {
    return sptrOf<FontCairo>(name, style, size, nullptr);
}

sptr<TextLayout> TextLayout::create(const std::wstring& src, const sptr<Font>& font) {
    return sptrOf<TextLayoutCairo>(src, std::static_pointer_cast<FontCairo>(font));
}

}  // namespace tex

namespace zk {

MicroTeXGraphics::MicroTeXGraphics(cairo_t* cr) : cr_(cr) {
    static FontCairo defaultFont("serif", tex::PLAIN, 20.f);
    cairo_get_matrix(cr_, &baseMatrix_);
    setColor(tex::black);
    setStroke(tex::Stroke());
    setFont(&defaultFont);
}

void MicroTeXGraphics::setColor(tex::color c) {
    color_ = c;
    const double a = tex::color_a(c) / 255.0;
    const double r = tex::color_r(c) / 255.0;
    const double g = tex::color_g(c) / 255.0;
    const double b = tex::color_b(c) / 255.0;
    cairo_set_source_rgba(cr_, r, g, b, a);
}

tex::color MicroTeXGraphics::getColor() const { return color_; }

void MicroTeXGraphics::setStroke(const tex::Stroke& s) {
    stroke_ = s;
    cairo_set_line_width(cr_, static_cast<double>(s.lineWidth));
    cairo_line_cap_t cap = CAIRO_LINE_CAP_ROUND;
    switch (s.cap) {
        case tex::CAP_BUTT: cap = CAIRO_LINE_CAP_BUTT; break;
        case tex::CAP_ROUND: cap = CAIRO_LINE_CAP_ROUND; break;
        case tex::CAP_SQUARE: cap = CAIRO_LINE_CAP_SQUARE; break;
    }
    cairo_set_line_cap(cr_, cap);

    cairo_line_join_t join = CAIRO_LINE_JOIN_ROUND;
    switch (s.join) {
        case tex::JOIN_BEVEL: join = CAIRO_LINE_JOIN_BEVEL; break;
        case tex::JOIN_ROUND: join = CAIRO_LINE_JOIN_ROUND; break;
        case tex::JOIN_MITER: join = CAIRO_LINE_JOIN_MITER; break;
    }
    cairo_set_line_join(cr_, join);
    cairo_set_miter_limit(cr_, static_cast<double>(s.miterLimit));
}

const tex::Stroke& MicroTeXGraphics::getStroke() const { return stroke_; }

void MicroTeXGraphics::setStrokeWidth(float w) {
    stroke_.lineWidth = w;
    cairo_set_line_width(cr_, static_cast<double>(w));
}

const tex::Font* MicroTeXGraphics::getFont() const { return font_; }

void MicroTeXGraphics::setFont(const tex::Font* font) { font_ = font; }

void MicroTeXGraphics::translate(float dx, float dy) {
    cairo_translate(cr_, static_cast<double>(dx), static_cast<double>(dy));
}

void MicroTeXGraphics::scale(float sx, float sy) {
    sx_ *= sx;
    sy_ *= sy;
    cairo_scale(cr_, static_cast<double>(sx), static_cast<double>(sy));
}

void MicroTeXGraphics::rotate(float angle) { cairo_rotate(cr_, static_cast<double>(angle)); }

void MicroTeXGraphics::rotate(float angle, float px, float py) {
    cairo_translate(cr_, static_cast<double>(px), static_cast<double>(py));
    cairo_rotate(cr_, static_cast<double>(angle));
    cairo_translate(cr_, static_cast<double>(-px), static_cast<double>(-py));
}

void MicroTeXGraphics::reset() {
    cairo_set_matrix(cr_, &baseMatrix_);
    sx_ = 1.f;
    sy_ = 1.f;
}

float MicroTeXGraphics::sx() const { return sx_; }
float MicroTeXGraphics::sy() const { return sy_; }

void MicroTeXGraphics::drawChar(wchar_t c, float x, float y) {
    const std::wstring s(1, c);
    drawText(s, x, y);
}

void MicroTeXGraphics::drawText(const std::wstring& t, float x, float y) {
    if (!font_ || t.empty()) return;
    const auto* f = static_cast<const FontCairo*>(font_);
    const std::string utf8 = tex::wide2utf8(t);
    if (f->face()) {
        cairo_set_font_face(cr_, f->face());
        cairo_set_font_size(cr_, static_cast<double>(f->getSize()));
        cairo_move_to(cr_, static_cast<double>(x), static_cast<double>(y));
        cairo_show_text(cr_, utf8.c_str());
        return;
    }
    PangoContext* ctx = pango_font_map_create_context(ftFontMap());
    PangoLayout* layout = pango_layout_new(ctx);
    pango_layout_set_text(layout, utf8.data(), static_cast<int>(utf8.size()));
    applyFontDesc(layout, f->family(), f->style(), f->getSize());
    PangoLayoutIter* it = pango_layout_get_iter(layout);
    const double ascent = static_cast<double>(pango_layout_iter_get_baseline(it)) / PANGO_SCALE;
    pango_layout_iter_free(it);
    cairo_save(cr_);
    cairo_translate(cr_, static_cast<double>(x), static_cast<double>(y) - ascent);
    pango_cairo_update_layout(cr_, layout);
    pango_cairo_show_layout(cr_, layout);
    cairo_restore(cr_);
    g_object_unref(layout);
    g_object_unref(ctx);
}

void MicroTeXGraphics::drawLine(float x1, float y1, float x2, float y2) {
    cairo_move_to(cr_, static_cast<double>(x1), static_cast<double>(y1));
    cairo_line_to(cr_, static_cast<double>(x2), static_cast<double>(y2));
    cairo_stroke(cr_);
}

void MicroTeXGraphics::drawRect(float x, float y, float w, float h) {
    cairo_rectangle(cr_, static_cast<double>(x), static_cast<double>(y), static_cast<double>(w), static_cast<double>(h));
    cairo_stroke(cr_);
}

void MicroTeXGraphics::fillRect(float x, float y, float w, float h) {
    cairo_rectangle(cr_, static_cast<double>(x), static_cast<double>(y), static_cast<double>(w), static_cast<double>(h));
    cairo_fill(cr_);
}

void MicroTeXGraphics::roundRectPath(float x, float y, float w, float h, float rx, float ry) {
    const double r = static_cast<double>(std::max(rx, ry));
    const double deg = kPi / 180.0;
    cairo_new_sub_path(cr_);
    cairo_arc(cr_, x + r, y + r, r, 180.0 * deg, 270.0 * deg);
    cairo_arc(cr_, x + w - r, y + r, r, -90.0 * deg, 0.0);
    cairo_arc(cr_, x + w - r, y + h - r, r, 0.0, 90.0 * deg);
    cairo_arc(cr_, x + r, y + h - r, r, 90.0 * deg, 180.0 * deg);
    cairo_close_path(cr_);
}

void MicroTeXGraphics::drawRoundRect(float x, float y, float w, float h, float rx, float ry) {
    roundRectPath(x, y, w, h, rx, ry);
    cairo_stroke(cr_);
}

void MicroTeXGraphics::fillRoundRect(float x, float y, float w, float h, float rx, float ry) {
    roundRectPath(x, y, w, h, rx, ry);
    cairo_fill(cr_);
}

}  // namespace zk
