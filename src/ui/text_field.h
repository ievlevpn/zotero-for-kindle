#pragma once
// The text of an input field, edited by key events (Kindle system keyboard or desktop keys).

#include <string>

#include "platform/input.h"

namespace zk {

class TextField {
public:
    void apply(const KeyEvent&);  // Text appends; Backspace removes one code point; Return submits
    const std::string& text() const { return text_; }
    void setText(std::string t) { text_ = std::move(t); submitted_ = false; }
    void clear() { setText({}); }
    bool submitted() const { return submitted_; }

private:
    std::string text_;
    bool submitted_ = false;
};

}  // namespace zk
