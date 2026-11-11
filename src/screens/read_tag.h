#pragma once
// The read tag (from Settings): the Zotero tag that marks an item read. A field with the current
// tag and, under it, the library's tags that contain what is typed; tapping one, or Return, keeps
// it (an empty field puts back "read"); Back leaves it as it was. Nothing is retagged.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "screens/screen.h"
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
class TagPickerScreen final : public Screen {
public:
    explicit TagPickerScreen(std::function<void(const std::string& tag)> chosen) : chosen_(std::move(chosen)) {}
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    bool wantsKeyboard() const override { return true; }
    bool showsTabBar() const override { return false; }

    const std::vector<std::pair<std::string, int64_t>>& suggestions() const { return shown_; }
    std::vector<Rect> suggestionRects() const;

private:
    void filter();
    void choose(const std::string& tag, Nav&);
    void leave(Nav&);

    std::function<void(const std::string&)> chosen_;
    TextField field_;
    std::vector<std::pair<std::string, int64_t>> tags_, shown_;
    int bottom_ = 0;
};

}  // namespace zk
