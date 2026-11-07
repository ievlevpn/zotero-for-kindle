#include "platform/scaled_display.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace zk {

ScaledDisplay::ScaledDisplay(std::unique_ptr<Display> screen, double scale)
    : screen_(std::move(screen)), scale_(scale), shrunk_(screen_->width(), screen_->height()) {}

int ScaledDisplay::width() const { return static_cast<int>(std::lround(screen_->width() / scale_)); }
int ScaledDisplay::height() const { return static_cast<int>(std::lround(screen_->height() / scale_)); }

Point ScaledDisplay::toApp(Point p) const {
    return {static_cast<int>(std::lround(p.x / scale_)), static_cast<int>(std::lround(p.y / scale_))};
}

void ScaledDisplay::present(const Canvas& canvas, Rect r, Waveform w) {
    const int sw = screen_->width(), sh = screen_->height();
    if (shrunk_.width() != sw || shrunk_.height() != sh) shrunk_.resize(sw, sh);  // turned
    // Every screen pixel the area touches.
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x * scale_)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y * scale_)));
    const int x1 = std::min(sw, static_cast<int>(std::ceil((r.x + r.w) * scale_)));
    const int y1 = std::min(sh, static_cast<int>(std::ceil((r.y + r.h) * scale_)));
    if (x1 <= x0 || y1 <= y0) return;
    const Rect onScreen{x0, y0, x1 - x0, y1 - y0};
    // Each screen pixel is the average of the app pixels it covers, weighted by how much of each
    // (cairo 1.10 on the firmware samples instead of averaging: hairlines and thin type break up).
    struct Tap {
        int from;
        std::vector<float> weights;
    };
    auto taps = [this](int dst, int srcSize) {
        const double a = dst / scale_, b = (dst + 1) / scale_;
        Tap t{static_cast<int>(std::floor(a)), {}};
        for (int s = t.from; s < b && s < srcSize; ++s)
            t.weights.push_back(static_cast<float>((std::min<double>(s + 1, b) - std::max<double>(s, a)) * scale_));
        return t;
    };
    std::vector<Tap> xs;
    for (int x = x0; x < x1; ++x) xs.push_back(taps(x, canvas.width()));
    cairo_surface_flush(canvas.surface());
    cairo_surface_flush(shrunk_.surface());
    const uint8_t* src = cairo_image_surface_get_data(canvas.surface());
    const int srcStride = cairo_image_surface_get_stride(canvas.surface());
    uint8_t* dst = cairo_image_surface_get_data(shrunk_.surface());
    const int dstStride = cairo_image_surface_get_stride(shrunk_.surface());
    std::vector<float> row(static_cast<size_t>(canvas.width()));
    for (int y = y0; y < y1; ++y) {
        const Tap ty = taps(y, canvas.height());
        // The covered app rows, averaged, for the app columns this area needs.
        const int c0 = xs.front().from, c1 = std::min(canvas.width(), xs.back().from + static_cast<int>(xs.back().weights.size()));
        std::fill(row.begin() + c0, row.begin() + c1, 0.f);
        for (size_t k = 0; k < ty.weights.size(); ++k) {
            const auto* line = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(ty.from + static_cast<int>(k)) * srcStride);
            for (int c = c0; c < c1; ++c) {
                const uint32_t p = line[c];
                row[c] += ty.weights[k] * static_cast<float>((((p >> 16) & 0xff) * 77 + ((p >> 8) & 0xff) * 150 + (p & 0xff) * 29) >> 8);
            }
        }
        auto* out = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * dstStride);
        for (int x = x0; x < x1; ++x) {
            const Tap& tx = xs[static_cast<size_t>(x - x0)];
            float v = 0;
            for (size_t k = 0; k < tx.weights.size(); ++k) v += tx.weights[k] * row[static_cast<size_t>(tx.from) + k];
            const uint32_t g = static_cast<uint32_t>(std::clamp(v + 0.5f, 0.f, 255.f));
            out[x] = (g << 16) | (g << 8) | g;
        }
    }
    cairo_surface_mark_dirty(shrunk_.surface());
    screen_->present(shrunk_, onScreen, w);
}

bool ScaledDisplay::pollEvents(std::vector<InputEvent>& out, int timeout_ms) {
    const size_t from = out.size();
    const bool more = screen_->pollEvents(out, timeout_ms);
    for (size_t i = from; i < out.size(); ++i) {
        out[i].touch.pos = toApp(out[i].touch.pos);
        out[i].pen.pos = toApp(out[i].pen.pos);
    }
    return more;
}

}  // namespace zk
