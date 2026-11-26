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

namespace {

void border(cairo_t* cr, Rect r, double width, double inset) {
    gray(cr, kInk);
    cairo_set_line_width(cr, width);
    cairo_rectangle(cr, r.x + inset + width / 2, r.y + inset + width / 2, r.w - 2 * inset - width, r.h - 2 * inset - width);
    cairo_stroke(cr);
}

}  // namespace

void bookCover(cairo_t* cr, Rect r, const std::string& title, const std::string& authors, uint8_t fill, uint8_t ink,
               uint8_t soft, uint8_t rule, bool framed, double titlePt, int pad) {
    fillRect(cr, r, fill);
    if (framed) {
        border(cr, r, 2, 0);
        cairo_set_line_width(cr, 1.5);
        const double ix = r.w * 0.05, iy = r.h * 0.05;
        cairo_rectangle(cr, r.x + ix + 0.75, r.y + iy + 0.75, r.w - 2 * ix - 1.5, r.h - 2 * iy - 1.5);
        cairo_stroke(cr);
    }
    const int ruleY = r.y + r.h - 112, titleY = r.y + pad + 6;
    drawText(cr, title, {kSerif, titlePt, 700, false, ink}, {r.x + pad, titleY, r.w - 2 * pad, ruleY - 16 - titleY}, 5);
    hRule(cr, ruleY, r.x + pad, r.x + pad + 60, rule);
    drawText(cr, authors, {kSans, 6.5, 400, false, soft}, {r.x + pad, r.y + r.h - 92, r.w - 2 * pad, 80}, 2);
}

void paperCover(cairo_t* cr, Rect r, const std::string& venue, const std::string& year, const std::string& title,
                const std::string& authors, double titlePt, int pad) {
    fillRect(cr, r, 255);
    border(cr, r, 2, 0);
    const int x = r.x + pad, w = r.w - 2 * pad, bottom = r.y + r.h - pad;
    int y = r.y + pad + 2;
    if (!venue.empty() || !year.empty()) {
        const TextStyle st{kSans, 4.5, 700, false, kSecondary, true};
        const auto tracked = [&](const std::string& s) {
            return "<span letter_spacing=\"" + std::to_string(static_cast<int>(st.size_pt * 0.14 * 1024)) + "\">" +
                   markupEscape(s) + "</span>";
        };
        // A long venue is cut; the year after it stays.
        const std::string tail = venue.empty() ? year : year.empty() ? "" : " " + year;
        const int tailW = tail.empty() ? 0 : measureText(tracked(tail), st, w, 1).width;
        TextResult v{};
        if (!venue.empty()) v = drawText(cr, tracked(venue), st, {x, y, w - tailW, bottom - y}, 1);
        if (!tail.empty()) {
            const TextResult t = drawText(cr, tracked(tail), st, {x + v.width, y, tailW + 4, bottom - y}, 1);
            v.height = std::max(v.height, t.height);
        }
        y += v.height + static_cast<int>(std::lround(r.w * 0.05));
    }
    y += drawText(cr, title, {kSerif, titlePt, 700}, {x, y, w, bottom - y}, 4).height;
    if (!authors.empty()) {
        y += static_cast<int>(std::lround(r.w * 0.03));
        y += drawText(cr, authors, {kSans, 5.5, 400, false, kSecondary}, {x, y, w, bottom - y}, 1).height;
    }
    const int thick = r.w >= 250 ? 4 : 3, gap = static_cast<int>(std::lround(r.w * 0.045));
    y += static_cast<int>(std::lround(r.w * 0.02));
    for (int i = 0; i < 5; ++i) {
        y += gap;
        if (y + thick > bottom) break;
        fillRect(cr, {x, y, i == 4 ? static_cast<int>(std::lround(r.w * 0.45)) : w, thick}, 207);
        y += thick;
    }
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

void lockIcon(cairo_t* cr, double cx, double cy, uint8_t g, double scale) {
    cairo_save(cr);
    iconPen(cr, g);
    cairo_set_line_width(cr, kIconStroke * scale);
    const double bw = 34 * scale, bh = 24 * scale, top = cy - 2 * scale;  // the body
    roundedRect(cr, cx - bw / 2, top, bw, bh, 4 * scale);
    cairo_stroke(cr);
    const double r = 10 * scale;  // the shackle
    cairo_move_to(cr, cx - r, top);
    cairo_line_to(cr, cx - r, top - 8 * scale);
    cairo_arc(cr, cx, top - 8 * scale, r, M_PI, 2 * M_PI);
    cairo_line_to(cr, cx + r, top);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void contentsIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    for (int i = -1; i <= 1; ++i) {
        const double y = cy + i * 13;
        cairo_move_to(cr, cx - 7, y);
        cairo_line_to(cr, cx + kIconR, y);
    }
    cairo_stroke(cr);
    for (int i = -1; i <= 1; ++i) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx - kIconR + 2, cy + i * 13, 3.5, 0, 2 * M_PI);
    }
    cairo_fill(cr);
    cairo_restore(cr);
}

void notesIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const double l = cx - 15.5, r = cx + 15.5, t = cy - kIconR, b = cy + kIconR, fold = 11;
    cairo_move_to(cr, r - fold, t);
    cairo_line_to(cr, l, t);
    cairo_line_to(cr, l, b);
    cairo_line_to(cr, r, b);
    cairo_line_to(cr, r, t + fold);
    cairo_close_path(cr);
    cairo_move_to(cr, r - fold, t);
    cairo_line_to(cr, r - fold, t + fold);
    cairo_line_to(cr, r, t + fold);
    cairo_move_to(cr, l + 8, cy + 2);
    cairo_line_to(cr, r - 8, cy + 2);
    cairo_move_to(cr, l + 8, cy + 11);
    cairo_line_to(cr, r - 12, cy + 11);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void displayIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    const TextStyle st{kSans, 10, 500, false, g};
    const TextResult m = measureText("Aa", st, 200, 1);
    drawText(cr, "Aa", st, {static_cast<int>(cx - m.width / 2.0), static_cast<int>(cy - m.height / 2.0), m.width + 4, m.height + 4}, 1);
}

void lightIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    cairo_arc(cr, cx, cy, 8, 0, 2 * M_PI);
    for (int i = 0; i < 8; ++i) {
        const double a = i * M_PI / 4;
        cairo_move_to(cr, cx + 15 * std::cos(a), cy + 15 * std::sin(a));
        cairo_line_to(cr, cx + kIconR * std::cos(a), cy + kIconR * std::sin(a));
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

void areaIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const double hw = kIconR, hh = 16, arm = 11;
    for (int sx : {-1, 1})
        for (int sy : {-1, 1}) {
            const double x = cx + sx * hw, y = cy + sy * hh;
            cairo_move_to(cr, x - sx * arm, y);
            cairo_line_to(cr, x, y);
            cairo_line_to(cr, x, y - sy * arm);
        }
    cairo_stroke(cr);
    cairo_restore(cr);
}

void stickyNoteIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const double h = 17.5, l = cx - h, r = cx + h, t = cy - h, b = cy + h, fold = 13;
    cairo_move_to(cr, l, t);
    cairo_line_to(cr, r, t);
    cairo_line_to(cr, r, b - fold);
    cairo_line_to(cr, r - fold, b);
    cairo_line_to(cr, l, b);
    cairo_close_path(cr);
    cairo_move_to(cr, r, b - fold);
    cairo_line_to(cr, r - fold, b - fold);
    cairo_line_to(cr, r - fold, b);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void saveIcon(cairo_t* cr, double cx, double cy, uint8_t g) {
    cairo_save(cr);
    iconPen(cr, g);
    const double hw = 17.5, t = cy - kIconR, b = cy + 16;
    // Downward arrow into a shallow tray ("Save to Zotero").
    cairo_move_to(cr, cx, t);
    cairo_line_to(cr, cx, cy + 5);
    cairo_move_to(cr, cx - 10, cy - 5);
    cairo_line_to(cr, cx, cy + 6);
    cairo_line_to(cr, cx + 10, cy - 5);
    cairo_move_to(cr, cx - hw, cy + 7);
    cairo_line_to(cr, cx - hw, b);
    cairo_line_to(cr, cx + hw, b);
    cairo_line_to(cr, cx + hw, cy + 7);
    cairo_stroke(cr);
    cairo_restore(cr);
}

}  // namespace zk
