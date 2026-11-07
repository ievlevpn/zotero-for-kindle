#include "ui/text_field.h"

namespace zk {

void TextField::apply(const KeyEvent& k) {
    switch (k.code) {
        case KeyEvent::Code::Text:
            text_ += k.text;
            submitted_ = false;
            break;
        case KeyEvent::Code::Backspace:
            // Drop UTF-8 continuation bytes (10xxxxxx), then the lead byte.
            while (!text_.empty() && (static_cast<unsigned char>(text_.back()) & 0xC0) == 0x80) text_.pop_back();
            if (!text_.empty()) text_.pop_back();
            submitted_ = false;
            break;
        case KeyEvent::Code::Return: submitted_ = true; break;
        case KeyEvent::Code::Escape: break;
    }
}

}  // namespace zk
