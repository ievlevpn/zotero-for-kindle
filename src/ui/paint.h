#pragma once
// Drawing primitives and icons shared by all screens (gray levels as in ui/theme.h).

#include <string>

#include <cairo.h>

#include "core/geometry.h"
#include "ui/text.h"

namespace zk {

void gray(cairo_t* cr, uint8_t g);
void fillRect(cairo_t* cr, Rect r, uint8_t g);
void roundedRect(cairo_t* cr, double x, double y, double w, double h, double radius);
void hRule(cairo_t* cr, int y, int x0, int x1, uint8_t g = 220);
// Text vertically centred in `box`.
void centredText(cairo_t* cr, const std::string& s, const TextStyle& st, Rect box, Align a = Align::Center);
// Circular "refresh" arrow: an open ring with a solid arrowhead continuing the curve.
void syncIcon(cairo_t* cr, double cx, double cy, double r);
void searchIcon(cairo_t* cr, double cx, double cy, uint8_t g);
void gearIcon(cairo_t* cr, double cx, double cy, double r, uint8_t g);
// Typographic cover for items without one: title in Bookerly, authors in Ember.
void generatedCover(cairo_t* cr, Rect r, const std::string& title, const std::string& authors, bool dark);
void typeBadge(cairo_t* cr, Rect r, const std::string& type);
void progressBar(cairo_t* cr, Rect r, double fraction);

}  // namespace zk
