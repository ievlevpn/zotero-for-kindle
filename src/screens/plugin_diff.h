#pragma once
// What to redraw when a plugin screen renders again: the top-level blocks of the screen's root
// that changed (or moved), each with the e-ink waveform the change calls for.

#include <vector>

#include "core/geometry.h"
#include "platform/display.h"
#include "screens/plugin_blocks.h"

namespace zk {

struct Damage {
    Rect rect;
    Waveform wave;
};

// Unchanged blocks: no damage. Text changed (a text or a notice) → Fast; a deck's card changed
// or a block turned its page → Quality; anything else (buttons, chips, a list's rows, a block
// added, removed or moved) → Ui. A changed block's old and new rects are redrawn; damage
// overlapping or a block gap apart is merged (the stronger waveform wins).
std::vector<Damage> diffTrees(const Laid& before, const Laid& after);

}  // namespace zk
