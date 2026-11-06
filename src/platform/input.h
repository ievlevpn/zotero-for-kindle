#pragma once

#include <cstdint>

#include "core/geometry.h"

namespace zk {

// One finger's touch sample, in screen pixels.
struct TouchEvent {
    enum class Type { Down, Move, Up };
    Type type;
    int id;  // finger (slot) id
    Point pos;
    int64_t t_ms;
};

}  // namespace zk
