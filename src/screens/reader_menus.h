#pragma once
// The reader's menus (spec §6.3 state 2): a top bar (‹ Library, title, search, contents) and a
// bottom panel (chapter + location, a progress slider, and Contents · Notes (n) · Display ·
// Light · Sync), drawn over the page. Opening/closing repaints only those regions.

#include <optional>
#include <string>
#include <vector>

#include "docs/document.h"
#include "screens/screen.h"

namespace zk {

class ReaderScreen;
class Lipc;

class ReaderMenus {
public:
    ReaderMenus(ReaderScreen& r, Lipc* lipc) : reader_(r), lipc_(lipc) {}

    void open(Nav&);
    void close(Nav&);
    void draw(Canvas&);
    void onGesture(const Gesture&, Nav&);
    void onKey(const KeyEvent&, Nav&) {}
    void onTick(Nav&) {}
    void onPage(Nav&);
    void onSyncEvent(const SyncEvent&, Nav&);
    void onSearchResults(const std::vector<SearchHit>&, Nav&) {}

    static Rect topBarRect();
    static Rect panelRect();
    static Rect backRect();
    static Rect searchRect();
    static Rect contentsIconRect();
    static Rect sliderRect();
    static Rect buttonRect(int i);  // 0 Contents, 1 Notes, 2 Display, 3 Light, 4 Sync
    static Rect lightPanelRect();
    static Rect lightSliderRect();
    static Rect displayPanelRect();
    static Rect fontRect(int i);  // up to 4 fonts per row; fontMoreRect() shows the next ones
    static Rect fontMoreRect();
    static Rect sizeMinusRect();
    static Rect sizePlusRect();
    static Rect marginRect(int i);   // EPUB: Narrow, Normal, Wide, Extra wide (a share of the page width)
    static Rect trimRect(int i);     // PDF, same row: 0 Keep the page's margins, 1 Trim them
    static Rect spacingRect(int i);  // 100, 120, 140 %
    static Rect orientationRect(int i);  // 0 Portrait, 1 Landscape, 2 Two columns (EPUB in landscape)
    static Rect penRect(int i);          // PDF once a pen was used: 0 Write, 1 Tap

    bool displayOpen() const { return displayOpen_; }
    bool showsTextOptions() const;
    bool showsPenOptions() const;
    const std::vector<std::string>& fonts();

    std::string notesLabel() const { return "Notes (" + std::to_string(notes_) + ")"; }
    bool lightOpen() const { return lightOpen_; }

private:
    void openContents(Nav&);
    void openNotes(Nav&);
    int sliderPage(int x) const;
    void setLight(int x, Nav&);

    ReaderScreen& reader_;
    Lipc* lipc_;
    size_t notes_ = 0;
    std::optional<int> dragPage_;
    bool lightOpen_ = false;
    bool displayOpen_ = false;
    size_t fontOffset_ = 0;
    bool onDisplayTap(Point, Nav&);
    void drawDisplay(cairo_t*);
    int light_ = -1;
    bool syncing_ = false;
    bool landscape_ = false;  // the setting, read when the menus open and changed by the row
};

}  // namespace zk
