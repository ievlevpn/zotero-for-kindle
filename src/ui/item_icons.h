#pragma once
// Monochrome icons for Zotero item types in list rows (book, article, preprint, web page…), drawn
// as line art for e-ink; the item's file format ("EPUB", "PDF", "WEB") goes under the icon when it has
// one. An item that opens a web snapshot ("WEB") shows the globe, whatever its type.

#include <string>

#include <cairo.h>

#include "core/geometry.h"

namespace zk {

// Draws into `box` (rows use 74 × 92): the icon in the top 64 px, the format label below it.
void itemTypeIcon(cairo_t*, Rect box, const std::string& itemType, const std::string& format);

}  // namespace zk
