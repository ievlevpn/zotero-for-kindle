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
    static Rect marginRect(int i);   // Narrow 40, Normal 60, Wide 100 px
    static Rect spacingRect(int i);  // 100, 120, 140 %

    bool displayOpen() const { return displayOpen_; }
    bool showsTextOptions() const;
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
    std::vector<std::string> fonts_;
    bool fontsLoaded_ = false;
    size_t fontOffset_ = 0;
    bool onDisplayTap(Point, Nav&);
    void drawDisplay(cairo_t*);
    int light_ = -1;
    bool syncing_ = false;
};

}  // namespace zk
