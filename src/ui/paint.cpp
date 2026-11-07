#include "ui/paint.h"

#include <algorithm>
#include <cmath>

#include "ui/theme.h"

namespace zk {

using namespace theme;

void gray(cairo_t* cr, uint8_t g) { cairo_set_source_rgb(cr, g / 255.0, g / 255.0, g / 255.0); }

void fillRect(cairo_t* cr, Rect r, uint8_t g) {
    gray(cr, g);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
}

void roundedRect(cairo_t* cr, double x, double y, double w, double h, double radius) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, x + radius, y + h - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void hRule(cairo_t* cr, int y, int x0, int x1, uint8_t g) { fillRect(cr, {x0, y, x1 - x0, 2}, g); }

void centredText(cairo_t* cr, const std::string& s, const TextStyle& st, Rect box, Align a) {
    TextResult m = measureText(s, st, box.w, 1);
    drawText(cr, s, st, {box.x, box.y + (box.h - m.height) / 2, box.w, m.height + 4}, 1, a);
}

void syncIcon(cairo_t* cr, double cx, double cy, double r) {
    const double start = -M_PI / 2 + 0.5, sweep = 1.55 * M_PI, end = start + sweep;
    gray(cr, kInk);
    cairo_new_path(cr);
    cairo_set_line_width(cr, 5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_arc(cr, cx, cy, r, start, end);
    cairo_stroke(cr);
    // Arrowhead at the end of the arc, pointing along the tangent (clockwise).
    double ex = cx + r * std::cos(end), ey = cy + r * std::sin(end);
    double tx = -std::sin(end), ty = std::cos(end);  // tangent
    double nx = std::cos(end), ny = std::sin(end);   // radial
    const double len = 15, half = 10;
    cairo_move_to(cr, ex + tx * len, ey + ty * len);
    cairo_line_to(cr, ex + nx * half, ey + ny * half);
    cairo_line_to(cr, ex - nx * half, ey - ny * half);
    cairo_close_path(cr);
    cairo_fill(cr);
}

void searchIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    gray(cr, g);
    cairo_set_line_width(cr, 5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, 15, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx + 11, cy + 11);
    cairo_line_to(cr, cx + 24, cy + 24);
    cairo_stroke(cr);
}

void generatedCover(cairo_t* cr, Rect r, const std::string& title, const std::string& authors, bool dark) {
    fillRect(cr, r, dark ? 42 : 255);
    if (!dark) {
        gray(cr, kInk);
        cairo_set_line_width(cr, 3);
        cairo_rectangle(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3);
        cairo_stroke(cr);
    }
    uint8_t ink = dark ? 255 : kInk, soft = dark ? 200 : kSecondary;
    int pad = 26;
    int ruleY = r.y + r.h - 112, titleY = r.y + pad + 6;
    drawText(cr, title, {kSerif, 9, 700, false, ink}, {r.x + pad, titleY, r.w - 2 * pad, ruleY - 16 - titleY}, 5);
    hRule(cr, ruleY, r.x + pad, r.x + pad + 60, soft);
    drawText(cr, authors, {kSans, 6.5, 400, false, soft}, {r.x + pad, r.y + r.h - 92, r.w - 2 * pad, 80}, 2);
}

void typeBadge(cairo_t* cr, Rect r, const std::string& type) {
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 6);
    cairo_stroke(cr);
    centredText(cr, type, {kSans, 5.5, 700}, r);
}

void progressBar(cairo_t* cr, Rect r, double fraction) {
    gray(cr, kRule);
    roundedRect(cr, r.x, r.y, r.w, r.h, r.h / 2.0);
    cairo_fill(cr);
    gray(cr, kInk);
    roundedRect(cr, r.x, r.y, std::max(double(r.h), r.w * fraction), r.h, r.h / 2.0);
    cairo_fill(cr);
}

void gearIcon(cairo_t* cr, double cx, double cy, double r, uint8_t g) {
    gray(cr, g);
    cairo_new_path(cr);
    const int teeth = 8;
    for (int i = 0; i < teeth * 2; ++i) {
        const double a0 = i * M_PI / teeth, a1 = (i + 1) * M_PI / teeth;
        const double rr = i % 2 ? r * 0.78 : r;
        cairo_arc(cr, cx, cy, rr, a0, a1);
    }
    cairo_close_path(cr);
    cairo_new_sub_path(cr);
    cairo_arc_negative(cr, cx, cy, r * 0.36, 2 * M_PI, 0);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
}

}  // namespace zk
