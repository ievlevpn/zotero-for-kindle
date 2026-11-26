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
void searchIcon(cairo_t* cr, double cx, double cy, uint8_t g);

// Header icons, one family: outlined, kIconStroke wide, round caps and joins, drawn inside a
// kIconSize square centred on (cx, cy), stroke included.
inline constexpr double kIconStroke = 5;
inline constexpr double kIconSize = 44;
// Sets the family's pen: gray `g`, kIconStroke, round caps and joins.
void iconPen(cairo_t* cr, uint8_t g);
// Two arcs chasing each other clockwise, each with an open arrowhead.
void syncIcon(cairo_t* cr, double cx, double cy, uint8_t g);
// Six outlined teeth around a round hole.
void gearIcon(cairo_t* cr, double cx, double cy, uint8_t g);
// A square with a knob on top and on the right; the whole shape is centred.
void puzzleIcon(cairo_t* cr, double cx, double cy, uint8_t g);
// A badge dot for puzzleIcon at the same centre: ink, with a white halo cutting the outline.
void puzzleBadge(cairo_t* cr, double cx, double cy);
void closeIcon(cairo_t* cr, double cx, double cy, uint8_t g);
// A padlock, closed (an account with a PIN, locked), in the same family at `scale` of its size.
void lockIcon(cairo_t* cr, double cx, double cy, uint8_t g, double scale = 1.0);
// The reader panel's: a bulleted list (Contents), a page with a folded corner and two lines
// (Notes), "Aa" (Display), a sun (Light), a frame's four corners (Mark area), and a square sticky
// note with its bottom-right corner folded (Add note).
void contentsIcon(cairo_t* cr, double cx, double cy, uint8_t g);
void notesIcon(cairo_t* cr, double cx, double cy, uint8_t g);
void displayIcon(cairo_t* cr, double cx, double cy, uint8_t g);
void lightIcon(cairo_t* cr, double cx, double cy, uint8_t g);
void areaIcon(cairo_t* cr, double cx, double cy, uint8_t g);
void stickyNoteIcon(cairo_t* cr, double cx, double cy, uint8_t g);
void saveIcon(cairo_t* cr, double cx, double cy, uint8_t g);
// The generated covers' painters (widgets.h, drawCover, picks one and its sizes). A book's: `fill`,
// its title in Bookerly bold in `ink` top-left, `pad` from the edges, a short rule in `rule` and its
// authors in Ember in `soft` bottom-left; `framed`: a 2 px ink border and a 1.5 px inner frame 5% in.
void bookCover(cairo_t* cr, Rect r, const std::string& title, const std::string& authors, uint8_t fill, uint8_t ink,
               uint8_t soft, uint8_t rule, bool framed, double titlePt, int pad);
// A paper's, a first page: white, a 2 px ink border; `venue` and `year` (in caps; "" for none) small,
// bold and tracked (a long venue is cut, the year stays), the title (Bookerly bold, at most 4 lines), the authors (Ember, small, 1 line),
// then five grey text lines, the last one short (as many as fit).
void paperCover(cairo_t* cr, Rect r, const std::string& venue, const std::string& year, const std::string& title,
                const std::string& authors, double titlePt, int pad);
void typeBadge(cairo_t* cr, Rect r, const std::string& type);
void progressBar(cairo_t* cr, Rect r, double fraction);

}  // namespace zk
