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

void iconPen(cairo_t* cr, uint8_t g) {
    gray(cr, g);
    cairo_new_path(cr);
    cairo_set_line_width(cr, kIconStroke);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

namespace {
// The radius of the stroke's centre line for a shape that fills the icon square.
constexpr double kIconR = (kIconSize - kIconStroke) / 2;  // 19.5
}  // namespace

void syncIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const double r = 15.5, head = 8.5, lead = 3;
    for (int k = 0; k < 2; ++k) {
        const double start = M_PI * (1 + k) + 0.45, end = M_PI * (2 + k) - 0.6;
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, start, end);
        // An open arrowhead a little ahead of the arc's end, its arms reaching back on both sides of the curve.
        const double tx = -std::sin(end), ty = std::cos(end);  // tangent, clockwise
        const double nx = std::cos(end), ny = std::sin(end);   // radial, outwards
        const double px = cx + r * nx + lead * tx, py = cy + r * ny + lead * ty;
        cairo_move_to(cr, px - head * (tx - nx), py - head * (ty - ny));
        cairo_line_to(cr, px, py);
        cairo_line_to(cr, px - head * (tx + nx), py - head * (ty + ny));
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

void gearIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const int teeth = 6;
    const double tip = kIconR, root = 14, tipHalf = 0.20, rootHalf = 0.30;
    for (int i = 0; i < teeth; ++i) {
        const double a = i * 2 * M_PI / teeth - M_PI / 2, next = a + 2 * M_PI / teeth;
        auto at = [&](double rr, double ang) { return std::pair{cx + rr * std::cos(ang), cy + rr * std::sin(ang)}; };
        auto [x0, y0] = at(root, a - rootHalf);
        i ? cairo_line_to(cr, x0, y0) : cairo_move_to(cr, x0, y0);
        cairo_arc(cr, cx, cy, tip, a - tipHalf, a + tipHalf);
        cairo_arc(cr, cx, cy, root, a + rootHalf, next - rootHalf);
    }
    cairo_close_path(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_restore(cr);
}

namespace {
// Puzzle geometry: a body of half-side kBody with knobs of radius kKnob centred kKnobOff
// beyond the top and right edges; the body is shifted so that body and knobs are centred.
constexpr double kBody = 13.5, kKnob = 6, kKnobOff = 4.5;
constexpr double kKnobOut = kKnobOff + kKnob;  // how far a knob reaches past its edge
}  // namespace

void puzzleIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const double bx = cx - kKnobOut / 2, by = cy + kKnobOut / 2, s = kBody;
    const double phi = std::asin(kKnobOff / kKnob);  // where a knob meets its edge
    cairo_move_to(cr, bx - s, by - s);
    cairo_arc(cr, bx, by - s - kKnobOff, kKnob, M_PI - phi, 2 * M_PI + phi);
    cairo_line_to(cr, bx + s, by - s);
    cairo_arc(cr, bx + s + kKnobOff, by, kKnob, 1.5 * M_PI - phi, 2.5 * M_PI + phi);
    cairo_line_to(cr, bx + s, by + s);
    cairo_line_to(cr, bx - s, by + s);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void puzzleBadge(cairo_t* cr, double cx, double cy) {
    const double x = cx - kKnobOut / 2 + kBody + 7, y = cy + kKnobOut / 2 - kBody - 7;
    gray(cr, 255);
    cairo_new_path(cr);
    cairo_arc(cr, x, y, 13, 0, 2 * M_PI);
    cairo_fill(cr);
    gray(cr, kInk);
    cairo_arc(cr, x, y, 9, 0, 2 * M_PI);
    cairo_fill(cr);
}

void closeIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const double r = 14;
    cairo_move_to(cr, cx - r, cy - r);
    cairo_line_to(cr, cx + r, cy + r);
    cairo_move_to(cr, cx + r, cy - r);
    cairo_line_to(cr, cx - r, cy + r);
    cairo_stroke(cr);
    cairo_restore(cr);
}

}  // namespace zk
