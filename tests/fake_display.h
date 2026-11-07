#pragma once
// A display for app tests: scripted input, recorded presents.

#include "platform/display.h"

#include <deque>
#include <vector>

namespace zk::test {

class FakeDisplay : public Display {
public:
    struct Present {
        Rect rect;
        Waveform waveform;
    };

    int width() const override { return 1236; }
    int height() const override { return 1547; }
    void present(const Canvas&, Rect r, Waveform w) override { presents.push_back({r, w}); }
    bool pollEvents(std::vector<InputEvent>& out, int) override {
        if (closed && input.empty()) return false;
        if (!input.empty()) {
            out.push_back(input.front());
            input.pop_front();
        }
        return true;
    }

    void touch(TouchEvent::Type type, Point p, int64_t t) {
        InputEvent e;
        e.touch = {type, 0, p, t};
        input.push_back(e);
    }
    void tap(Point p, int64_t t) {
        touch(TouchEvent::Type::Down, p, t);
        touch(TouchEvent::Type::Up, p, t + 60);
    }
    void key(KeyEvent k) {
        InputEvent e;
        e.kind = InputEvent::Kind::Key;
        e.key = std::move(k);
        input.push_back(e);
    }
    void lifecycle(InputEvent::Kind kind) {
        InputEvent e;
        e.kind = kind;
        input.push_back(e);
    }

    std::deque<InputEvent> input;
    std::vector<Present> presents;
    bool closed = false;
};

}  // namespace zk::test
