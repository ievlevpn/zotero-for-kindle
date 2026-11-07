#include "ui/text.h"

#include <algorithm>

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

// Only pango <= 1.26 and cairo <= 1.10 API: that is what the Kindle firmware ships.

namespace zk {

namespace {

constexpr double kDpi = 300.0;

// FreeType/fontconfig font map on every platform, so the desktop renders like the device.
// Recreated after addFontDir: a map caches the font set it was created with.
PangoFontMap*& fontMapSlot() {
    static PangoFontMap* map = nullptr;
    return map;
}

PangoFontMap* fontMap() {
    PangoFontMap*& map = fontMapSlot();
    if (!map) map = pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    return map;
}

struct Layout {
    PangoContext* ctx;
    PangoLayout* layout;
    Layout(PangoContext* c, PangoLayout* l) : ctx(c), layout(l) {}
    Layout(const Layout&) = delete;
    Layout(Layout&& o) noexcept : ctx(o.ctx), layout(o.layout) { o.ctx = nullptr, o.layout = nullptr; }
    ~Layout() {
        if (layout) g_object_unref(layout);
        if (ctx) g_object_unref(ctx);
    }
};

void configure(Layout& l, cairo_t* cr, std::string_view utf8, const TextStyle& style, int width,
               int max_lines, Align align) {
    if (cr) pango_cairo_update_context(cr, l.ctx);
    pango_cairo_context_set_resolution(l.ctx, kDpi);
    pango_layout_context_changed(l.layout);

    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, style.family);
    pango_font_description_set_size(desc, static_cast<int>(style.size_pt * PANGO_SCALE));
    pango_font_description_set_weight(desc, static_cast<PangoWeight>(style.weight));
    pango_font_description_set_style(desc, style.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(l.layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_width(l.layout, width * PANGO_SCALE);
    pango_layout_set_wrap(l.layout, PANGO_WRAP_WORD_CHAR);
    if (max_lines > 0) {
        pango_layout_set_height(l.layout, -max_lines);
        pango_layout_set_ellipsize(l.layout, PANGO_ELLIPSIZE_END);
    }
    switch (align) {
        case Align::Left: pango_layout_set_alignment(l.layout, PANGO_ALIGN_LEFT); break;
        case Align::Center: pango_layout_set_alignment(l.layout, PANGO_ALIGN_CENTER); break;
        case Align::Right: pango_layout_set_alignment(l.layout, PANGO_ALIGN_RIGHT); break;
        case Align::Justify:
            pango_layout_set_alignment(l.layout, PANGO_ALIGN_LEFT);
            pango_layout_set_justify(l.layout, TRUE);
            break;
    }
    pango_layout_set_text(l.layout, utf8.data(), static_cast<int>(utf8.size()));
}

Layout makeLayout(cairo_t* cr, std::string_view utf8, const TextStyle& style, int width, int max_lines,
                  Align align) {
    PangoContext* ctx = pango_font_map_create_context(fontMap());
    Layout l{ctx, pango_layout_new(ctx)};
    configure(l, cr, utf8, style, width, max_lines, align);
    return l;
}

TextResult resultOf(PangoLayout* layout, std::string_view utf8) {
    TextResult r;
    if (utf8.empty()) return r;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    r.width = logical.width;
    r.height = logical.height;
    r.lines = pango_layout_get_line_count(layout);
    r.truncated = pango_layout_is_ellipsized(layout);
    return r;
}

}  // namespace

void addFontDir(const std::string& dir) {
    FcConfigAppFontAddDir(FcConfigGetCurrent(), reinterpret_cast<const FcChar8*>(dir.c_str()));
    // Text drawn before this call created a font map with the old font set: start a new one.
    PangoFontMap*& map = fontMapSlot();
    if (map) {
        g_object_unref(map);  // live contexts keep their own reference
        map = nullptr;
    }
}

TextResult drawText(cairo_t* cr, std::string_view utf8, const TextStyle& style, Rect box, int max_lines,
                    Align align) {
    if (utf8.empty()) return {};
    Layout l = makeLayout(cr, utf8, style, box.w, max_lines, align);
    // The box height is a limit too: drop lines (ellipsizing the last) until the text fits.
    for (TextResult m = resultOf(l.layout, utf8); m.height > box.h && max_lines > 1; m = resultOf(l.layout, utf8)) {
        max_lines = std::min(max_lines, m.lines) - 1;
        configure(l, cr, utf8, style, box.w, max_lines, align);
    }
    double g = style.gray / 255.0;
    cairo_save(cr);
    cairo_set_source_rgb(cr, g, g, g);
    cairo_move_to(cr, box.x, box.y);
    pango_cairo_show_layout(cr, l.layout);
    cairo_restore(cr);
    cairo_new_path(cr);  // save/restore keeps the path: don't leave a current point behind
    return resultOf(l.layout, utf8);
}

TextResult measureText(std::string_view utf8, const TextStyle& style, int width, int max_lines) {
    if (utf8.empty()) return {};
    Layout l = makeLayout(nullptr, utf8, style, width, max_lines, Align::Left);
    return resultOf(l.layout, utf8);
}

std::string resolvedFamily(std::string_view utf8, const TextStyle& style) {
    if (utf8.empty()) return {};
    Layout l = makeLayout(nullptr, utf8, style, 100000, 1, Align::Left);
    PangoLayoutLine* line = pango_layout_get_line_readonly(l.layout, 0);
    if (!line || !line->runs) return {};
    auto* run = static_cast<PangoGlyphItem*>(line->runs->data);
    PangoFontDescription* desc = pango_font_describe(run->item->analysis.font);
    std::string family = pango_font_description_get_family(desc);
    pango_font_description_free(desc);
    return family;
}

}  // namespace zk
