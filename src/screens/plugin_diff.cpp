#include "screens/plugin_diff.h"

#include <algorithm>

namespace zk {

namespace {

using K = Block::Kind;

bool same(const Laid& a, const Laid& b) {
    return a.rect == b.rect && a.first == b.first && a.count == b.count && sameBlock(*a.block, *b.block);
}

Waveform waveFor(const Laid* a, const Laid* b, bool pageTurned) {
    if (pageTurned) return Waveform::Quality;
    if (!a || !b || a->block->kind != b->block->kind) return Waveform::Ui;
    if (a->first != b->first && sameBlock(*a->block, *b->block)) return Waveform::Quality;  // the block paged
    switch (a->block->kind) {
        case K::Deck: return a->block->children == b->block->children ? Waveform::Fast : Waveform::Quality;
        case K::Card: return Waveform::Quality;
        case K::Text:
        case K::Notice: return a->rect == b->rect ? Waveform::Fast : Waveform::Ui;
        default: return Waveform::Ui;
    }
}

// Side by side, or one above the other with no more than the largest gap between blocks.
bool touching(Rect a, Rect b) {
    constexpr int kGap = 60;
    return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h + kGap && b.y <= a.y + a.h + kGap;
}

}  // namespace

std::vector<Damage> diffTrees(const Laid& before, const Laid& after) {
    std::vector<Damage> out;
    const bool pageTurned = before.page != after.page;
    const size_t n = std::max(before.children.size(), after.children.size());
    for (size_t i = 0; i < n; ++i) {
        const Laid* a = i < before.children.size() ? &before.children[i] : nullptr;
        const Laid* b = i < after.children.size() ? &after.children[i] : nullptr;
        if (a && b && same(*a, *b)) continue;
        const Rect r = (a ? a->rect : Rect{}).united(b ? b->rect : Rect{});
        if (!r.empty()) out.push_back({r, waveFor(a, b, pageTurned)});
    }
    if (before.pages != after.pages || pageTurned) {
        const Rect r = pageIndicatorRect(before).united(pageIndicatorRect(after));
        if (!r.empty()) out.push_back({r, pageTurned ? Waveform::Quality : Waveform::Ui});
    }
    // Overlapping or neighbouring damage becomes one rect, with the stronger waveform.
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < out.size() && !merged; ++i)
            for (size_t j = i + 1; j < out.size() && !merged; ++j)
                if (touching(out[i].rect, out[j].rect)) {
                    out[i].rect = out[i].rect.united(out[j].rect);
                    out[i].wave = std::max(out[i].wave, out[j].wave);
                    out.erase(out.begin() + static_cast<long>(j));
                    merged = true;
                }
    }
    return out;
}

}  // namespace zk
