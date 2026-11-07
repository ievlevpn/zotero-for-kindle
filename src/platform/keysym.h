#pragma once
// X11 keysyms (what the Kindle's on-screen keyboard sends) -> key events with UTF-8 text.

#include <optional>

#include "platform/input.h"

namespace zk {

std::optional<KeyEvent> keyFromKeysym(unsigned long keysym);

}  // namespace zk
