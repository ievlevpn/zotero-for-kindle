#pragma once
// Reusable pieces of the screens (mockup C): list rows, tab bar, chips, banner, toggle,
// buttons, page indicator. Each draws into a rect; the matching *Rect functions hit-test.

#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "net/errors.h"
#include "store/library.h"

namespace zk {

constexpr int kMargin = 60;
constexpr int kHeaderH = 140;
constexpr int kTabBarH = 130;
constexpr int kRowH = 186;
constexpr int kRowsPerPage = 6;
constexpr int kPageIndicatorH = 60;

// Badge, Bookerly title (2 lines), byline, and on the right "✓ Read" or the progress ("71%", "new", "opened")
// unless `status` is set ("Downloading 42%", or an error message, shown in place of the byline).
void drawItemRow(cairo_t*, Rect, const ItemRow&, const std::string& status);

void drawTabBar(cairo_t*, Rect bar, const std::vector<std::string>& tabs, int active);
Rect tabRect(Rect bar, int index, int count);

void drawChips(cairo_t*, Rect area, const std::vector<std::string>& labels, int active);
std::vector<Rect> chipRects(Rect area, const std::vector<std::string>& labels);  // stops when out of room

// The error message (up to 2 lines) and a small "Details: …" line.
void drawBanner(cairo_t*, Rect, const UserError&);
void drawToggle(cairo_t*, Rect, bool on);
void drawButton(cairo_t*, Rect, const std::string& label, bool primary);
void drawPageIndicator(cairo_t*, Rect, int page /* 0-based */, int pages);  // "3 / 41"

// Screen header: large title, or "‹" + title for pushed screens; the back target is backRect().
void drawHeader(cairo_t*, const std::string& title, bool back, int rightReserve = 0);  // px kept free on the right
Rect headerBackRect();

// A dark pill with a short message.
void drawToast(cairo_t*, Rect, const std::string& text);

// A highlight or note: shade bar, text (Bookerly, 3 lines), comment, page label, and its sync
// state ("not synced yet", "on the Kindle only", or a conflict with Re-apply · Discard).
constexpr int kNoteRowH = 220;
// conflictButtons: only where a screen handles Re-apply · Discard (the Notes tab).
void drawNoteRow(cairo_t*, Rect, const AnnotationRecord&, bool conflictButtons = false);
std::string noteSyncMark(const AnnotationRecord&);  // "" when it is simply in Zotero
std::string noteRowLabel(const AnnotationRecord&);  // an area: "Area · p. 12"; "" for the rest
Rect noteRowButtonRect(Rect row, int i);            // a conflict's 0 Re-apply, 1 Discard

// A collection line: folder glyph, name, item count, "›" when it has subcollections.
// `pinned`: a pin before the count; `kept` (kept downloaded): a download arrow before that;
// `note` (a pinned subcollection's "in Economics", "Downloading 3 of 12") under the name.
void drawCollectionRow(cairo_t*, Rect, const std::string& name, int64_t items, bool hasChildren, bool pinned = false,
                       const std::string& note = "", bool kept = false);
// Where drawCollectionRow puts the name, and its marks (left to right: kept, pinned).
Rect collectionRowNameRect(Rect row, bool pinned, bool kept);
std::vector<Rect> collectionRowMarks(Rect row, bool pinned, bool kept);

}  // namespace zk
