#pragma once
// The sheet a long-press on an item slides up over a list (Home, Library, a collection, search):
// title, people, collections, the file and the start of the abstract, with Mark as read (or unread)
// above Open / Remove from Kindle / More…. The host draws it over its list and passes it the
// gestures while it is open.

#include <string>

#include <cairo.h>

#include "screens/screen.h"
#include "store/library.h"

namespace zk {

class ItemSheet {
public:
    enum class Action { None, Close, Open, Changed, More };

    ItemSheet(ItemRow, ItemDetails);
    // Fades `area` and draws the sheet over its bottom.
    void draw(cairo_t*, Rect area) const;
    // Close: a tap above the sheet. Changed: the file was just removed, or the item marked read or
    // unread (the host reloads its rows).
    Action onGesture(const Gesture&, Rect area, Nav&);

    Rect sheetRect(Rect area) const;
    Rect openRect(Rect area) const;
    Rect removeRect(Rect area) const;  // only drawn while the file is on the Kindle
    Rect moreRect(Rect area) const;
    Rect readRect(Rect area) const;  // Mark as read / Mark as unread

    const ItemRow& row() const { return row_; }
    const ItemDetails& details() const { return details_; }
    bool confirmingRemove() const { return confirmRemove_; }

private:
    const ItemDetails::File* file() const;  // the one the row opens
    int contentHeight(int width) const;

    ItemRow row_;
    ItemDetails details_;
    bool confirmRemove_ = false;
};

// Shared with the details page.
std::string itemTypeLabel(const std::string& itemType);  // "journalArticle" -> "Journal article"
std::string peopleLine(const ItemRecord&, int max);        // "Jane Austen and Vivien Jones"
// "EPUB · 0.8 MB · on this Kindle · 13% read · 2 highlights"
std::string fileLine(const ItemDetails::File&, double progress);

}  // namespace zk
