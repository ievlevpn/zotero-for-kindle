#pragma once
// The read tag (from Settings): the Zotero tag that marks an item read. A field with the current
// tag and, under it, the library's tags that contain what is typed; tapping one, or Return, keeps
// it (an empty field puts back "read"); Back leaves it as it was. Nothing is retagged.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "screens/screen.h"
#include "store/library.h"
#include "ui/text_field.h"

namespace zk {

class ReadTagScreen final : public Screen {
public:
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    bool wantsKeyboard() const override { return true; }
    bool showsTabBar() const override { return false; }

    const std::vector<std::pair<std::string, int64_t>>& suggestions() const { return shown_; }  // tag, items
    std::vector<Rect> suggestionRects() const;
    const std::string& text() const { return field_.text(); }

private:
    void filter();
    void save(const std::string& tag, Nav&);
    void leave(Nav&);

    TextField field_;
    std::vector<std::pair<std::string, int64_t>> tags_, shown_;
    int bottom_ = 0;  // above the keyboard
};

// The Library's Tag filter: the same field and list, every tag of the library (most used first)
// that contains what is typed. Tapping one, or Return (the first shown), chooses it; Back
// chooses nothing.
//
// Options make it the annotations' picker too (0.7): the Notes' Tag filter (the tags on
// annotations, "12 notes"), and the annotation bar's + Tag, which lists the library's tags with
// the recent ones first and offers "Add “xyz”" at the top while the typed tag is none of them
// (Return then keeps what is typed).
class TagPickerScreen final : public Screen {
public:
    using Tags = std::vector<std::pair<std::string, int64_t>>;  // tag, count (0: no count shown)
    struct Options {
        std::function<Tags(Library&)> source;  // none: the library's item tags
        std::string placeholder = "Search tags";
        std::string note = "Show only the items with this tag.";
        const char* unit = "item";  // "3 items"
        bool addNew = false;
        bool reading = false;  // over the reader or its panels: turns with them
    };
    explicit TagPickerScreen(std::function<void(const std::string& tag)> chosen) : chosen_(std::move(chosen)) {}
    TagPickerScreen(std::function<void(const std::string& tag)> chosen, Options o)
        : chosen_(std::move(chosen)), options_(std::move(o)) {}
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    bool wantsKeyboard() const override { return true; }
    bool showsTabBar() const override { return false; }
    bool isReading() const override { return options_.reading; }

    const std::vector<std::pair<std::string, int64_t>>& suggestions() const { return shown_; }
    std::vector<Rect> suggestionRects() const;
    // "Add “xyz”": the typed tag (trimmed) when it is new; "" when there is no such row.
    std::string newTag() const;
    Rect addRect() const;  // the Add row (empty if none)
    const std::string& text() const { return field_.text(); }

private:
    void filter();
    void choose(const std::string& tag, Nav&);
    void leave(Nav&);

    std::function<void(const std::string&)> chosen_;
    Options options_;
    TextField field_;
    std::vector<std::pair<std::string, int64_t>> tags_, shown_;
    int bottom_ = 0;
};

// The Tag filter of the Notes tab and a book's notes: the tags on annotations (of that file;
// "" every one), most used first, "12 notes".
TagPickerScreen::Options annotationTagFilter(const std::string& attachmentKey, bool reading = false);

}  // namespace zk
