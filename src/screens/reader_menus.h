#pragma once
// The reader's menus (spec §6.3 state 2): a top bar (‹ Library, title, search, contents) and a
// bottom panel (chapter + location, a progress slider, and Contents · Notes (n) · Display ·
// Light · Sync), drawn over the page. Opening/closing repaints only those regions.

#include <optional>
#include <string>
#include <vector>

#include "docs/document.h"
#include "screens/screen.h"
#include "ui/text.h"

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
    void onTick(Nav&);
    void onPage(Nav&);
    void onSyncEvent(const SyncEvent&, Nav&);
    void onSearchResults(const std::vector<SearchHit>&, Nav&) {}

    static Rect topBarRect();
    static Rect panelRect();
    static Rect backRect();
    static Rect searchRect();
    static Rect contentsIconRect();
    static Rect markAreaRect();  // PDF: "Mark area", the sixth cell
    static Rect addNoteRect();   // PDF: "Add note", the seventh
    static Rect sliderRect();
    // Where the slider takes a finger: its track, a little taller (a ~9 mm target) and wider.
    static Rect sliderHitRect();
    static Rect pageLabelRect();  // "Page 12 of 300", above the slider's right end
    // A touch going down here drags at once (the app asks): the slider follows the finger.
    bool dragsFrom(Point at) const { return !picker_ && sliderHitRect().contains(at); }
    static constexpr int kDragFrameMs = 100;  // the slider redraws at most ~10 times a second
    // 0 Contents, 1 Notes, 2 Display, 3 Light, 4 Sync, 5 Mark area, 6 Add note (PDF), of `count`.
    static Rect buttonRect(int i, int count = 5);
    int buttonCount() const;  // 7 on a PDF (Mark area, Add note), else 5
    // A cell's label: 6.5 pt, or down to 5.5 pt by halves until it fits `room` px.
    static TextStyle labelStyle(const std::string& label, int room);
    static Rect lightPanelRect();
    static Rect lightSliderRect();
    static Rect displayPanelRect();
    static Rect fontRowRect();  // "Font  Bookerly ›": opens the font pop-up
    static Rect sizeMinusRect();
    static Rect sizePlusRect();
    static Rect marginRect(int i);   // EPUB: Narrow, Normal, Wide, Extra wide (a share of the page width)
    static Rect pdfViewRect(int i);  // PDF, same row: 0 the Page, 1 Trim its margins, 2 Columns
    static Rect spacingRect(int i);  // 100, 120, 140 %
    static Rect orientationRect(int i);  // 0 Portrait, 1 Landscape, 2 Two columns (EPUB in landscape)
    static Rect penRect(int i);
    static Rect penWidthRect(int i);     // PDF once a pen was used: 0 Thin, 1 Medium, 2 Thick          // PDF once a pen was used: 0 Write, 1 Tap

    bool displayOpen() const { return displayOpen_; }
    bool showsTextOptions() const;
    bool showsPenOptions() const;
    const std::vector<std::string>& fonts();  // the Fast fonts last
    // Tests: the fonts to offer instead of the ones found (a Kindle's, on the desktop); empty: found.
    static void setFontsForTests(std::vector<std::string>);

    // The font pop-up: every font, its name in its own face, a page at a time.
    bool fontPickerOpen() const { return picker_.has_value(); }
    Rect fontPickerRect() const;
    std::vector<std::pair<std::string, Rect>> fontPickerRows();  // this page's fonts
    Rect fontPickerPrevRect();  // empty on the first page
    Rect fontPickerNextRect();  // empty on the last
    bool fontPickerHeadingShown();  // "FAST READING" on this page

    std::string pageLabel() const;  // "Page 12 of 300": the page under the finger while dragging
    bool sliderDragged() const { return dragPage_.has_value(); }
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
    std::optional<int> dragPage_;  // the page under the finger while the slider is dragged
    int64_t dragShownMs_ = 0;       // when the slider was last redrawn during the drag
    bool dragBehind_ = false;       // it moved since: redrawn once kDragFrameMs have passed
    void showDrag(Nav&);
    bool lightOpen_ = false;
    bool displayOpen_ = false;
    std::optional<size_t> picker_;  // the font pop-up's page
    // A page of the pop-up: its fonts, fonts()[from, to).
    struct PickerPage {
        size_t from = 0, to = 0;
    };
    std::vector<PickerPage> pickerPages();
    size_t pickerPage();     // picker_, clamped to the pages there are now
    // The "FAST READING" heading goes above fonts()[i] on a page starting at `from`: the group's
    // first, or its first on a continuation page.
    bool headingAbove(size_t i, size_t from);
    Rect shownPickerRect();  // the box as drawn
    void onPickerTap(Point, Nav&);
    void drawPicker(cairo_t*);
    bool onDisplayTap(Point, Nav&);
    void drawDisplay(cairo_t*);
    int light_ = -1;
    bool syncing_ = false;
    bool landscape_ = false;  // the setting, read when the menus open and changed by the row
};

}  // namespace zk
