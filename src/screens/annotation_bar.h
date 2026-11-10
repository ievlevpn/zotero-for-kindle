#pragma once
// The bar over a selection or a tapped highlight (spec §6.3 state 3): Zotero's 8 colours as
// gray dots with their names, then Highlight · Underline · Note (new) or Comment · Delete
// (existing). Picking a colour only selects it; the buttons commit.
//
// When some plugin has selection actions, the Create bar has a fourth button, More…: it turns
// the bar into a short list of those actions (title, the plugin's name dimmed beside it). While
// one runs, More… reads "Working…".

#include <array>
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

class AnnotationBar {
public:
    enum class Mode { Create, Edit };
    enum class Action { None, Colour, Highlight, Underline, Note, Comment, Delete, More, Plugin };
    struct Hit {
        Action action = Action::None;
        int colour = -1;  // Colour: the dot; Plugin: the action's index
    };

    // `anchor`: the selection or highlight; the bar sits above it, or below if there's no room.
    // `more`: the More… button (Create only; some plugin has selection actions).
    AnnotationBar(Mode, Rect anchor, std::string colour, bool more = false);

    Mode mode() const { return mode_; }
    Rect rect() const { return rect_; }
    Rect dotRect(int i) const;     // 0..7, Zotero's order
    Rect buttonRect(int i) const;  // Create: 0 Highlight, 1 Underline, 2 Note; Edit: 0 Comment, 1 Delete
    const std::string& colour() const { return colour_; }
    void setColour(std::string c) { colour_ = std::move(c); }
    // More… tapped: the bar lists these instead (empty: back to the buttons).
    void showActions(std::vector<PluginAction>);
    bool listing() const { return !actions_.empty(); }
    Rect actionRect(int i) const;  // a listed action's row
    void setWorking(bool w) { working_ = w; }
    bool working() const { return working_; }

    void draw(cairo_t*) const;
    Hit hit(Point) const;

    static constexpr int kH = 250;

private:
    int buttons() const { return mode_ == Mode::Create ? 3 + more_ : 2; }
    void place(int h);

    Mode mode_;
    Rect anchor_, rect_;
    std::string colour_;
    bool more_ = false, working_ = false;
    std::vector<PluginAction> actions_;
};

class Nav;

// Editing an existing annotation (spec §6.3 "tapping an existing highlight"): the Edit bar
// (a colour recolours at once), its comment in a field over the keyboard, and "Delete this
// highlight?" before deleting. Changes go to the library as edits to be sent; the caller
// refreshes the page's highlights when changed() and repaints the page when it closes.
class AnnotationEditor {
public:
    enum class Result { Open, Closed };

    AnnotationEditor(AnnotationRecord, Rect anchor);

    const std::string& key() const { return annotation_.key; }
    const AnnotationBar& bar() const { return bar_; }
    bool confirming() const { return state_ == State::Confirm; }
    bool commenting() const { return state_ == State::Comment; }
    bool wantsKeyboard() const { return commenting(); }
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
};

}  // namespace zk
