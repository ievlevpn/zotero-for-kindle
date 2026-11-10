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

    // The Scribe's wet ink: `failInk` makes the next requests fail (the handler didn't answer).
    bool hasInk = false, inkOn = false, failInk = false;
    bool systemInk() const override { return hasInk; }
    bool setSystemInk(bool on) override {
        if (failInk) return false;
        inkOn = on;
        return true;
    }
    int width() const override { return w; }
    int height() const override { return h; }
    int w = 1236, h = 1547;  // set before the App is made (its canvas takes this size)
    Orientation orientation = Orientation::Portrait;
    // Turning swaps the window's sides, then reports it like the Kindle does.
    void setOrientation(Orientation o) override {
        if (o == orientation) return;
        orientation = o;
        std::swap(w, h);
        InputEvent e;
        e.kind = InputEvent::Kind::Resize;
        input.push_back(e);
    }
    void present(const Canvas&, Rect r, Waveform w) override { presents.push_back({r, w}); }
    // As the Kindle's display: back in front, it has repainted the window itself (Expose).
    bool repaints = false;
    bool repaintsOnForeground() const override { return repaints; }
    bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) override {
        timeouts.push_back(timeout_ms);
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
    void pen(PenEvent::Type type, Point p, int64_t t, bool eraser = false) {
        InputEvent e;
        e.kind = InputEvent::Kind::Pen;
        e.pen.type = type;
        e.pen.pos = p;
        e.pen.t_ms = t;
        e.pen.eraser = eraser;
        input.push_back(e);
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
    std::vector<int> timeouts;  // what each pollEvents was allowed to wait (-1: no limit)
    bool closed = false;
};

}  // namespace zk::test
