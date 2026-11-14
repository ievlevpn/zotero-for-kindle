#pragma once
// The bar over a selection or a tapped highlight (spec §6.3 state 3): Zotero's 8 colours as
// gray dots with their names, the tag row (0.7: the four tags put on annotations most recently
// as chips, then + Tag), then Highlight · Underline · Note (new) or Comment · Delete (existing).
// Picking a colour only selects it; the buttons commit. On a new selection a chip only marks the
// tag for the highlight about to be made; on an existing annotation it adds or removes it at once.
//
// When some plugin has selection actions, the Create bar has a fourth button, More…: it turns
// the bar into a short list of those actions (title, the plugin's name dimmed beside it). While
// one runs, More… reads "Working…".
//
// Text (a web snapshot or a read-only group library, where nothing is highlighted): no colours,
// only Search, and More… when some plugin has selection actions.
//
// View (an annotation the Kindle may not change: another member's in a group, or any in a
// read-only group): one quiet line, "by Anna" (or why it can't be changed), nothing to tap.

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "platform/input.h"
#include "screens/plugin_entries.h"
#include "store/records.h"

namespace zk {

struct ZoteroColour {
    const char* hex;
    const char* name;
};
const std::array<ZoteroColour, 8>& zoteroColours();

class Library;
class Nav;

// The tag row (the annotation bar's, the sticky note pop-up's): a chip per recent tag (filled:
// the annotation has it), then + Tag. Chips are at least 9 mm tall; each is as wide as its tag,
// up to an equal share of the row (a long tag is cut with "…"), so the row fits any Kindle.
class TagRow {
public:
    static constexpr int kChips = 4;
    static constexpr int kH = 110;  // 9.3 mm
    static constexpr int kAdd = 1000;  // hit(): + Tag

    TagRow() = default;
    TagRow(std::vector<std::string> chips, std::vector<std::string> on);
    void place(Rect row);
    Rect rect() const { return rect_; }
    const std::vector<std::string>& chips() const { return chips_; }
    const std::vector<std::string>& on() const { return on_; }
    bool isOn(const std::string& tag) const;
    void setOn(std::vector<std::string> on) { on_ = std::move(on); }
    // The chips as now (after + Tag chose a tag, which leads them then).
    void setChips(std::vector<std::string> chips);
    Rect chipRect(int i) const { return chipRects_[static_cast<size_t>(i)]; }
    Rect addRect() const { return addRect_; }
    void draw(cairo_t*) const;
    int hit(Point) const;  // a chip's index, kAdd, or -1

private:
    std::vector<std::string> chips_, on_;
    Rect rect_, addRect_;
    std::vector<Rect> chipRects_;
};

// Another member's annotation in a group: their name (Zotero's createdByUser), for "by Anna" in
// lists and on the bar. "" for the user's own, anything in My Library, and an unnamed author.
std::string annotationAuthor(Library&, const AnnotationRecord&);
// "" when the Kindle may change the annotation (Library::annotationEditable); otherwise what the
// bar says instead: "by Anna", or "This group library is read-only".
std::string annotationReadOnly(Library&, const AnnotationRecord&);
constexpr const char* kReadOnlyGroup = "This group library is read-only";

// The chips' tags: the first TagRow::kChips of the tags put on annotations most recently here.
std::vector<std::string> recentChips(Library&);
// Adds `tag` to the annotation as it is now in the library (on), or takes it off; an added tag
// becomes the most recent. Saved as an edit to be sent (plugins told). Returns the annotation's
// tags afterwards; nullopt if it is gone (deleted, or a sync removed it).
std::optional<std::vector<std::string>> setAnnotationTag(Nav&, const std::string& key, const std::string& tag, bool on);
// + Tag: the tag picker over the screen (the library's tags, recent ones first, and "Add "xyz""
// for a new one); `reading`: opened from the reader or its panels (it turns with them).
void pickAnnotationTag(Nav&, std::function<void(const std::string& tag)> chosen, bool reading = true);

class AnnotationBar {
public:
    enum class Mode { Create, Edit, Text, View };
    enum class Action { None, Colour, Highlight, Underline, Note, Comment, Delete, More, Plugin, Tag, AddTag, Search };
    struct Hit {
        Action action = Action::None;
        int colour = -1;  // Colour: the dot; Plugin: the action's index; Tag: the chip's
    };

    // `anchor`: the selection or highlight; the bar sits above it, or below if there's no room.
    // `more`: the More… button (Create and Text; some plugin has selection actions). `tags`: its tag
    // row (none in Text: nothing is highlighted there).
    AnnotationBar(Mode, Rect anchor, std::string colour, bool more = false, TagRow tags = {});

    Mode mode() const { return mode_; }
    Rect rect() const { return rect_; }
    Rect dotRect(int i) const;     // 0..7, Zotero's order (none in Text)
    Rect buttonRect(int i) const;  // Create: 0 Highlight, 1 Underline, 2 Note; Edit: 0 Comment, 1 Delete; Text: 0 Search
    bool hasColours() const { return mode_ == Mode::Create || mode_ == Mode::Edit; }
    // View: the line it shows ("by Anna").
    const std::string& label() const { return label_; }
    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& colour() const { return colour_; }
    void setColour(std::string c) { colour_ = std::move(c); }
    // More… tapped: the bar lists these instead (empty: back to the buttons).
    void showActions(std::vector<PluginAction>);
    bool listing() const { return !actions_.empty(); }
    const std::vector<PluginAction>& listed() const { return actions_; }  // what the list shows
    Rect actionRect(int i) const;  // a listed action's row
    void setWorking(bool w) { working_ = w; }
    bool working() const { return working_; }
    const TagRow& tags() const { return tags_; }
    TagRow& tags() { return tags_; }
    // A new selection's: a chip tapped marks or unmarks its tag (Create).
    void toggleTag(const std::string& tag);

    void draw(cairo_t*) const;
    Hit hit(Point) const;

    static constexpr int kH = 250 + 12 + TagRow::kH;
    static constexpr int kTextH = 124;  // Text: the buttons' row alone

private:
    int buttons() const {
        return mode_ == Mode::Create ? 3 + more_ : mode_ == Mode::Text ? 1 + more_ : mode_ == Mode::View ? 0 : 2;
    }
    int height() const { return mode_ == Mode::Text || mode_ == Mode::View ? kTextH : kH; }
    void place(int h);

    Mode mode_;
    Rect anchor_, rect_;
    std::string colour_, label_;
    bool more_ = false, working_ = false;
    std::vector<PluginAction> actions_;
    TagRow tags_;
};

// Editing an existing annotation (spec §6.3 "tapping an existing highlight"): the Edit bar
// (a colour recolours at once), its comment in a field over the keyboard, and "Delete this
// highlight?" before deleting. Changes go to the library as edits to be sent; the caller
// refreshes the page's highlights when changed() and repaints the page when it closes.
class AnnotationEditor {
public:
    enum class Result { Open, Closed };

    // `chips`: the tag row's (recentChips()). `readOnly` (annotationReadOnly()): the annotation can't
    // be changed here; the bar only says so ("by Anna"), and the comment and Delete don't open.
    AnnotationEditor(AnnotationRecord, Rect anchor, std::vector<std::string> chips = {}, std::string readOnly = "");
    ~AnnotationEditor();

    const std::string& key() const { return annotation_.key; }
    const AnnotationBar& bar() const { return bar_; }
    const AnnotationRecord& annotation() const { return annotation_; }
    bool readOnly() const { return bar_.mode() == AnnotationBar::Mode::View; }
    bool confirming() const { return state_ == State::Confirm; }
    bool commenting() const { return state_ == State::Comment; }
    bool wantsKeyboard() const { return commenting(); }
    // Straight to the comment, or to the confirmation (a sticky note's pop-up): there, Cancel closes.
    void editComment(Nav&);
    void confirmDelete(Nav&);
    // The confirmation's: "Delete this highlight?" (an area: "Delete this area?", a note: "Delete this note?")
    std::string question() const;
    Rect rect() const;              // what is drawn now: the bar, the question or the comment box
    Rect confirmRect(int i) const;  // 0 Delete, 1 Cancel
    static Rect commentRect();                // above the device's keyboard (when lipc says nothing)
    static Rect commentRectAbove(Rect keyboard);  // above this keyboard; commentRect() if it is empty

    // True once since the last call if the annotation changed (the page's highlights need redoing).
    bool takeChanged();

    void draw(cairo_t*) const;
    Result onTap(Point, Nav&);
    Result onKey(const KeyEvent&, Nav&);

private:
    enum class State { Bar, Confirm, Comment };
    void enter(State, Nav&);

    AnnotationRecord annotation_;
    AnnotationBar bar_;
    State state_ = State::Bar;
    std::string comment_;
    Rect commentBox_;  // placed when the comment opens, above the keyboard as it is then
    bool changed_ = false;
    bool direct_ = false;  // opened at the comment or the confirmation: no bar to go back to
    std::shared_ptr<AnnotationEditor*> self_;  // + Tag's picker: this editor, or null once it is gone
};

}  // namespace zk
