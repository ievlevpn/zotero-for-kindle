#include "ui/item_icons.h"

#include <cmath>

#include "ui/paint.h"
#include "ui/theme.h"

namespace zk {

using namespace theme;

namespace {

constexpr double kLine = 3.5;

void stroke(cairo_t* cr) {
    gray(cr, kInk);
    cairo_set_line_width(cr, kLine);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
}

void line(cairo_t* cr, double x0, double y0, double x1, double y1) {
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
}

// A page with a folded top-right corner in the icon square (x, y, s = side).
struct Page {
    double x, y, w, h;
};
Page page(cairo_t* cr, double cx, double top, double s, bool dashed = false) {
    const Page p{cx - s * 0.36, top + s * 0.02, s * 0.72, s * 0.96};
    const double fold = s * 0.2;
    if (dashed) {
        const double d[] = {7, 5};
        cairo_set_dash(cr, d, 2, 0);
    }
    cairo_move_to(cr, p.x, p.y);
    cairo_line_to(cr, p.x + p.w - fold, p.y);
    cairo_line_to(cr, p.x + p.w, p.y + fold);
    cairo_line_to(cr, p.x + p.w, p.y + p.h);
    cairo_line_to(cr, p.x, p.y + p.h);
    cairo_close_path(cr);
    stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
    line(cr, p.x + p.w - fold, p.y, p.x + p.w - fold, p.y + fold);
    line(cr, p.x + p.w - fold, p.y + fold, p.x + p.w, p.y + fold);
    stroke(cr);
    return p;
}

void textLines(cairo_t* cr, const Page& p, double from, int n, bool titleBar = false) {
    const double x0 = p.x + p.w * 0.18, x1 = p.x + p.w * 0.82;
    double y = p.y + p.h * from;
    if (titleBar) {
        cairo_set_line_width(cr, kLine * 2);
        line(cr, x0, y, x1 - p.w * 0.12, y);
        gray(cr, kInk);
        cairo_stroke(cr);
        y += p.h * 0.16;
    }
    for (int i = 0; i < n; ++i, y += p.h * 0.13) line(cr, x0, y, i == n - 1 ? (x0 + x1) / 2 : x1, y);
    stroke(cr);
}

void book(cairo_t* cr, double cx, double top, double s, bool ribbon) {
    const double w = s * 0.7, h = s * 0.94, x = cx - w / 2, y = top + s * 0.03;
    roundedRect(cr, x, y, w, h, 4);
    stroke(cr);
    gray(cr, kInk);
    cairo_rectangle(cr, x, y, w * 0.2, h);  // the spine
    cairo_fill(cr);
    line(cr, x + w * 0.36, y + h * 0.22, x + w * 0.84, y + h * 0.22);  // title
    line(cr, x + w * 0.36, y + h * 0.34, x + w * 0.72, y + h * 0.34);
    stroke(cr);
    if (ribbon) {
        const double rx = x + w * 0.66, rw = w * 0.16;
        cairo_move_to(cr, rx, y);
        cairo_line_to(cr, rx, y + h * 0.62);
        cairo_line_to(cr, rx + rw / 2, y + h * 0.52);
        cairo_line_to(cr, rx + rw, y + h * 0.62);
        cairo_line_to(cr, rx + rw, y);
        cairo_close_path(cr);
        gray(cr, kInk);
        cairo_fill(cr);
    }
}

void globe(cairo_t* cr, double cx, double cy, double r) {
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    stroke(cr);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, 0.45, 1);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_restore(cr);
    stroke(cr);
    line(cr, cx - r, cy, cx + r, cy);
    line(cr, cx - r * 0.85, cy - r * 0.5, cx + r * 0.85, cy - r * 0.5);
    line(cr, cx - r * 0.85, cy + r * 0.5, cx + r * 0.85, cy + r * 0.5);
    stroke(cr);
}

}  // namespace

void itemTypeIcon(cairo_t* cr, Rect box, const std::string& t, const std::string& format) {
    cairo_save(cr);
    cairo_rectangle(cr, box.x, box.y, box.w, box.h);
    cairo_clip(cr);
    cairo_new_path(cr);
    const double s = 64, cx = box.x + box.w / 2.0, top = box.y + 2;

    if (t == "book") {
        book(cr, cx, top, s, false);
    } else if (t == "bookSection") {
        book(cr, cx, top, s, true);
    } else if (t == "journalArticle" || t == "magazineArticle") {
        textLines(cr, page(cr, cx, top, s), 0.2, 4, true);
    } else if (t == "newspaperArticle") {
        const Page p = page(cr, cx, top, s);
        cairo_set_line_width(cr, kLine * 2);
        line(cr, p.x + p.w * 0.16, p.y + p.h * 0.2, p.x + p.w * 0.7, p.y + p.h * 0.2);
        gray(cr, kInk);
        cairo_stroke(cr);
        for (int col = 0; col < 2; ++col)
            for (int i = 0; i < 4; ++i) {
                const double x0 = p.x + p.w * (col ? 0.54 : 0.16), y = p.y + p.h * (0.4 + i * 0.13);
                line(cr, x0, y, x0 + p.w * 0.3, y);
            }
        stroke(cr);
    } else if (t == "preprint") {
        textLines(cr, page(cr, cx, top, s, true), 0.24, 4);
    } else if (t == "conferencePaper" || t == "presentation") {
        const Page p = page(cr, cx, top, s);
        textLines(cr, p, 0.22, 2);
        // A lectern.
        const double lx = p.x + p.w * 0.5, ly = p.y + p.h * 0.56;
        cairo_move_to(cr, lx - p.w * 0.26, ly);
        cairo_line_to(cr, lx + p.w * 0.26, ly);
        cairo_line_to(cr, lx + p.w * 0.16, ly + p.h * 0.32);
        cairo_line_to(cr, lx - p.w * 0.16, ly + p.h * 0.32);
        cairo_close_path(cr);
        gray(cr, kInk);
        cairo_fill(cr);
    } else if (t == "thesis") {
        const double cy = top + s * 0.38;
        cairo_move_to(cr, cx - s * 0.46, cy);
        cairo_line_to(cr, cx, cy - s * 0.2);
        cairo_line_to(cr, cx + s * 0.46, cy);
        cairo_line_to(cr, cx, cy + s * 0.2);
        cairo_close_path(cr);
        gray(cr, kInk);
        cairo_fill(cr);
        cairo_move_to(cr, cx - s * 0.26, cy + s * 0.14);
        cairo_line_to(cr, cx - s * 0.26, cy + s * 0.42);
        cairo_curve_to(cr, cx - s * 0.1, cy + s * 0.54, cx + s * 0.1, cy + s * 0.54, cx + s * 0.26, cy + s * 0.42);
        cairo_line_to(cr, cx + s * 0.26, cy + s * 0.14);
        line(cr, cx + s * 0.4, cy + s * 0.04, cx + s * 0.4, cy + s * 0.4);  // the tassel
        stroke(cr);
    } else if (t == "report") {
        const Page p = page(cr, cx, top, s);
        textLines(cr, p, 0.22, 1);
        gray(cr, kInk);
        const double base = p.y + p.h * 0.84, bw = p.w * 0.14;
        const double heights[] = {0.2, 0.36, 0.28};
        for (int i = 0; i < 3; ++i)
            cairo_rectangle(cr, p.x + p.w * (0.2 + i * 0.22), base - p.h * heights[i], bw, p.h * heights[i]);
        cairo_fill(cr);
    } else if (t == "webpage") {
        globe(cr, cx, top + s / 2, s * 0.42);
    } else if (t == "blogPost" || t == "forumPost") {
        roundedRect(cr, cx - s * 0.44, top + s * 0.08, s * 0.88, s * 0.62, 8);
        stroke(cr);
        cairo_move_to(cr, cx - s * 0.2, top + s * 0.7);
        cairo_line_to(cr, cx - s * 0.32, top + s * 0.92);
        cairo_line_to(cr, cx - s * 0.02, top + s * 0.7);
        stroke(cr);
        for (int i = 0; i < 3; ++i) line(cr, cx - s * 0.3, top + s * (0.24 + i * 0.15), cx + s * (i == 2 ? 0.1 : 0.3), top + s * (0.24 + i * 0.15));
        stroke(cr);
    } else if (t == "letter" || t == "email") {
        cairo_rectangle(cr, cx - s * 0.46, top + s * 0.18, s * 0.92, s * 0.64);
        stroke(cr);
        line(cr, cx - s * 0.46, top + s * 0.18, cx, top + s * 0.54);
        line(cr, cx, top + s * 0.54, cx + s * 0.46, top + s * 0.18);
        stroke(cr);
    } else if (t == "attachment") {
        const Page p = page(cr, cx, top, s);
        textLines(cr, p, 0.6, 2);
        // A paper clip over the top left.
        const double px = p.x + p.w * 0.22, py = p.y - 2;
        cairo_move_to(cr, px, py + s * 0.36);
        cairo_line_to(cr, px, py + s * 0.06);
        cairo_arc(cr, px + s * 0.07, py + s * 0.06, s * 0.07, M_PI, 2 * M_PI);
        cairo_line_to(cr, px + s * 0.14, py + s * 0.3);
        cairo_arc_negative(cr, px + s * 0.1, py + s * 0.3, s * 0.04, 0, M_PI);
        cairo_line_to(cr, px + s * 0.06, py + s * 0.1);
        stroke(cr);
    } else if (t == "patent") {
        const double cy = top + s * 0.4;
        cairo_move_to(cr, cx - s * 0.18, cy + s * 0.2);
        cairo_line_to(cr, cx - s * 0.26, cy + s * 0.56);
        cairo_line_to(cr, cx - s * 0.1, cy + s * 0.46);
        cairo_line_to(cr, cx, cy + s * 0.58);
        cairo_line_to(cr, cx + s * 0.1, cy + s * 0.46);
        cairo_line_to(cr, cx + s * 0.26, cy + s * 0.56);
        cairo_line_to(cr, cx + s * 0.18, cy + s * 0.2);
        stroke(cr);
        cairo_arc(cr, cx, cy, s * 0.3, 0, 2 * M_PI);
        gray(cr, 255);
        cairo_fill_preserve(cr);
        stroke(cr);
        cairo_arc(cr, cx, cy, s * 0.16, 0, 2 * M_PI);
        gray(cr, kInk);
        cairo_fill(cr);
    } else if (t == "computerProgram") {
        roundedRect(cr, cx - s * 0.44, top + s * 0.12, s * 0.88, s * 0.76, 6);
        stroke(cr);
        cairo_move_to(cr, cx - s * 0.14, top + s * 0.32);
        cairo_line_to(cr, cx - s * 0.3, top + s * 0.5);
        cairo_line_to(cr, cx - s * 0.14, top + s * 0.68);
        cairo_move_to(cr, cx + s * 0.14, top + s * 0.32);
        cairo_line_to(cr, cx + s * 0.3, top + s * 0.5);
        cairo_line_to(cr, cx + s * 0.14, top + s * 0.68);
        line(cr, cx + s * 0.06, top + s * 0.28, cx - s * 0.06, top + s * 0.72);
        stroke(cr);
    } else if (t == "dataset") {
        const double rx = s * 0.36, ry = s * 0.12, y0 = top + s * 0.16, y1 = top + s * 0.84;
        for (double y : {y0, (y0 + y1) / 2}) {
            cairo_save(cr);
            cairo_translate(cr, cx, y);
            cairo_scale(cr, rx, ry);
            cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
            cairo_restore(cr);
            stroke(cr);
        }
        cairo_save(cr);
        cairo_translate(cr, cx, y1);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1, 0, M_PI);
        cairo_restore(cr);
        line(cr, cx - rx, y0, cx - rx, y1);
        line(cr, cx + rx, y0, cx + rx, y1);
        stroke(cr);
    } else if (t == "statute" || t == "case" || t == "bill" || t == "hearing") {
        const Page p = page(cr, cx, top, s);
        centredText(cr, "§", {kSerif, 13, 700}, {static_cast<int>(p.x), static_cast<int>(p.y + p.h * 0.2),
                                                  static_cast<int>(p.w), static_cast<int>(p.h * 0.7)});
    } else if (t == "manuscript") {
        const Page p = page(cr, cx, top, s);
        for (int i = 0; i < 4; ++i) {
            const double y = p.y + p.h * (0.3 + i * 0.15), x0 = p.x + p.w * 0.18, x1 = p.x + p.w * 0.82;
            cairo_move_to(cr, x0, y);
            cairo_curve_to(cr, x0 + (x1 - x0) * 0.3, y - 5, x0 + (x1 - x0) * 0.6, y + 5, x1, y);
        }
        stroke(cr);
    } else {  // document and anything newer: a plain page
        textLines(cr, page(cr, cx, top, s), 0.3, 4);
    }

    if (!format.empty())
        centredText(cr, format, {kSans, 5, 700, false, kSecondary}, {box.x, box.y + 66, box.w, box.h - 66});
    cairo_restore(cr);
}

}  // namespace zk
