#pragma once

#include <vector>

#include "platform/input.h"

namespace zk {

// LongPressUp: the finger lifted after a long press without moving (a drag ends with DragEnd).
enum class GestureType { Tap, LongPress, DragStart, DragMove, DragEnd, Swipe, LongPressUp };
enum class Direction { None, Left, Right, Up, Down };

struct Gesture {
    GestureType type;
    Point pos;    // current position (Down position for Tap/LongPress)
    Point start;  // where the touch went down
    Direction dir = Direction::None;
    int64_t downMs = 0;  // when the touch went down (the input's clock)
    int finger = -1;     // the touch id (App feeds a pen stroke used as a finger as App::kPenFinger)
};

struct GestureConfig {
    int slop_px = 36;  // ~3 mm at 300 dpi: a tap may wobble that much
    int longpress_ms = 500;
    int swipe_min_px = 120;
    int swipe_max_ms = 1000;  // an unhurried swipe counts (big screens invite slower ones)
};

// Turns raw touches into gestures. Tracks only the first finger down;
// other fingers are ignored until it lifts.
class GestureRecognizer {
public:
    explicit GestureRecognizer(GestureConfig config = {});

    std::vector<Gesture> feed(const TouchEvent& event);

    // Call periodically while a finger is down; emits LongPress when due.
    std::vector<Gesture> tick(int64_t now_ms);
    // When tick() has something to emit (the long press of a finger held still); -1: nothing.
    int64_t nextDueMs() const { return state_ == State::Pressed ? start_ms_ + config_.longpress_ms : -1; }

private:
    enum class State { Idle, Pressed, Moving, LongPressed, Dragging };

    GestureConfig config_;
    State state_ = State::Idle;
    int finger_ = -1;
    Point start_{};
    int64_t start_ms_ = 0;
};

}  // namespace zk
