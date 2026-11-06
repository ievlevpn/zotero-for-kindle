#include "platform/kindle/x_touch.h"

namespace zk {

std::optional<TouchEvent> touchFromX(XPointerEvent kind, unsigned button, bool button1Down, Point pos, int64_t t_ms) {
    switch (kind) {
        case XPointerEvent::Press:
            if (button != 1) return std::nullopt;
            return TouchEvent{TouchEvent::Type::Down, 0, pos, t_ms};
        case XPointerEvent::Release:
            if (button != 1) return std::nullopt;
            return TouchEvent{TouchEvent::Type::Up, 0, pos, t_ms};
        case XPointerEvent::Motion:
            if (!button1Down) return std::nullopt;
            return TouchEvent{TouchEvent::Type::Move, 0, pos, t_ms};
    }
    return std::nullopt;
}

}  // namespace zk
