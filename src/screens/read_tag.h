#pragma once
// The read tag (from Settings): the Zotero tag that marks an item read. A field with the current
// tag and, under it, the library's tags that contain what is typed; tapping one, or Return, keeps
// it (an empty field puts back "read"); Back leaves it as it was. Nothing is retagged.

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

}  // namespace zk
