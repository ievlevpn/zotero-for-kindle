#include "ui/gesture.h"

#include <cstdlib>

namespace zk {

namespace {

bool beyond(Point a, Point b, int slop) {
    int dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy > slop * slop;
}

}  // namespace

GestureRecognizer::GestureRecognizer(GestureConfig config) : config_(config) {}

std::vector<Gesture> GestureRecognizer::feed(const TouchEvent& e) {
    std::vector<Gesture> out;

    if (state_ == State::Idle) {
        if (e.type == TouchEvent::Type::Down) {
            state_ = State::Pressed;
            finger_ = e.id;
            start_ = e.pos;
            start_ms_ = e.t_ms;
        }
        return out;
    }
    if (e.id != finger_) return out;

    // Movement first: an Up can arrive without any Move before it.
    if (e.type != TouchEvent::Type::Down && beyond(e.pos, start_, config_.slop_px)) {
        if (state_ == State::Pressed) {
            state_ = State::Moving;
        } else if (state_ == State::LongPressed) {
            state_ = State::Dragging;
            out.push_back({GestureType::DragStart, e.pos, start_});
        }
    }
    if (e.type == TouchEvent::Type::Move && state_ == State::Dragging) {
        out.push_back({GestureType::DragMove, e.pos, start_});
    }

    if (e.type == TouchEvent::Type::Up) {
        switch (state_) {
            case State::Pressed:
                out.push_back({GestureType::Tap, start_, start_});
                break;
            case State::Moving: {
                int dx = e.pos.x - start_.x, dy = e.pos.y - start_.y;
                bool horizontal = std::abs(dx) >= std::abs(dy);
                int distance = horizontal ? std::abs(dx) : std::abs(dy);
                if (e.t_ms - start_ms_ <= config_.swipe_max_ms && distance >= config_.swipe_min_px) {
                    Direction dir = horizontal ? (dx < 0 ? Direction::Left : Direction::Right)
                                               : (dy < 0 ? Direction::Up : Direction::Down);
                    out.push_back({GestureType::Swipe, e.pos, start_, dir});
                }
                break;
            }
            case State::Dragging:
                out.push_back({GestureType::DragEnd, e.pos, start_});
                break;
            case State::LongPressed:
            case State::Idle:
                break;
        }
        state_ = State::Idle;
        finger_ = -1;
    }
    return out;
}

std::vector<Gesture> GestureRecognizer::tick(int64_t now_ms) {
    std::vector<Gesture> out;
    if (state_ == State::Pressed && now_ms - start_ms_ >= config_.longpress_ms) {
        state_ = State::LongPressed;
        out.push_back({GestureType::LongPress, start_, start_});
    }
    return out;
}

}  // namespace zk
