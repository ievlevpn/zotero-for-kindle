#include "platform/color.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "platform/display.h"
#include "ui/canvas.h"

namespace zk {

namespace {

constexpr int kChromaAbove = 24;  // max - min of R, G, B

int chroma(uint32_t p) {
    const int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
    return std::max({r, g, b}) - std::min({r, g, b});
}

// More than 0.5% of `total`.
bool enoughColored(long colored, long total) { return total > 0 && colored * 200 > total; }
// The fewest colored pixels that are enough: counting can stop there.
long coloredNeeded(long total) { return total / 200 + 1; }

// Where a channel goes in a mask: its lowest bit and how many bits it has.
struct Channel {
    int shift = 0;
    uint32_t max = 0;  // the mask's value range, shifted down
    explicit Channel(uint32_t mask) {
        if (!mask) return;
        shift = std::countr_zero(mask);
        max = mask >> shift;
    }
    uint32_t put(uint32_t v8) const { return ((v8 * max + 127) / 255) << shift; }
};

// Whole-byte channels (the usual 24-bit visuals): a value is only shifted into place.
bool byteChannels(const Channel& r, const Channel& g, const Channel& b) { return r.max == 255 && g.max == 255 && b.max == 255; }

const uint32_t* canvasRow(const Canvas& c, int y) {
    const uint8_t* data = cairo_image_surface_get_data(c.surface());
    return reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(y) * cairo_image_surface_get_stride(c.surface()));
}

}  // namespace

ColorCaps decideColorCaps(bool panelFound, bool settingOn, const char* zkColor) {
    bool panel = panelFound;
    if (zkColor && std::strcmp(zkColor, "0") == 0) panel = false;
    if (zkColor && std::strcmp(zkColor, "1") == 0) panel = true;
    return {panel, panel && settingOn};
}

const char* colorPathName(ColorPath p) {
    switch (p) {
        case ColorPath::X: return "x";
        case ColorPath::Fb: return "fb";
        case ColorPath::Gray: break;
    }
    return "gray";
}

ColorPath chooseColorPath(const ColorCaps& caps, bool xTrueColor24, bool fbWritable, const char* forced) {
    if (!caps.on) return ColorPath::Gray;
    if (forced && std::strcmp(forced, "fb") == 0 && fbWritable) return ColorPath::Fb;
    if (forced && std::strcmp(forced, "x") == 0 && xTrueColor24) return ColorPath::X;
    if (xTrueColor24) return ColorPath::X;
    if (fbWritable) return ColorPath::Fb;
    return ColorPath::Gray;
}

bool isColored(const uint32_t* rgb24, int stride, Rect r) {
    if (r.empty()) return false;
    long colored = 0;
    const long need = coloredNeeded(static_cast<long>(r.w) * r.h);
    for (int y = r.y; y < r.y + r.h && colored < need; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(rgb24) + static_cast<size_t>(y) * stride);
        for (int x = r.x; x < r.x + r.w; ++x) colored += chroma(row[x]) > kChromaAbove;
    }
    return enoughColored(colored, static_cast<long>(r.w) * r.h);
}

const char* colorWfmName(ColorWfm w) {
    switch (w) {
        case ColorWfm::Gl16: return "GL16";
        case ColorWfm::Glrc16: return "GLRC16";
        case ColorWfm::Gc16: return "GC16";
        case ColorWfm::Gcc16: return "GCC16";
        case ColorWfm::Auto: return "AUTO";
        case ColorWfm::Keep: break;
    }
    return "keep";
}

ColorWfm colorWaveform(Waveform w, bool colored) {
    switch (w) {
        case Waveform::Ui: return colored ? ColorWfm::Glrc16 : ColorWfm::Gl16;
        case Waveform::Quality:
        case Waveform::Full: return colored ? ColorWfm::Gcc16 : ColorWfm::Gc16;
        case Waveform::Fast:
        case Waveform::Ink: break;
    }
    return ColorWfm::Keep;
}

bool toXrgb(const Canvas& c, uint8_t* dst, int dstStride, Rect r, uint32_t rmask, uint32_t gmask, uint32_t bmask) {
    r = r.intersected({0, 0, c.width(), c.height()});
    if (r.empty()) return false;
    cairo_surface_flush(c.surface());
    const Channel cr(rmask), cg(gmask), cb(bmask);
    const bool bytes = byteChannels(cr, cg, cb);
    long colored = 0;
    const long need = coloredNeeded(static_cast<long>(r.w) * r.h);  // once there, rows aren't counted
    for (int y = r.y; y < r.y + r.h; ++y) {
        const uint32_t* src = canvasRow(c, y) + r.x;
        auto* out = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * dstStride) + r.x;
        if (colored < need)
            for (int x = 0; x < r.w; ++x) colored += chroma(src[x]) > kChromaAbove;
        if (bytes) {
            for (int x = 0; x < r.w; ++x) {
                const uint32_t p = src[x];
                out[x] = ((p >> 16) & 0xff) << cr.shift | ((p >> 8) & 0xff) << cg.shift | (p & 0xff) << cb.shift;
            }
        } else {
            for (int x = 0; x < r.w; ++x) {
                const uint32_t p = src[x];
                out[x] = cr.put((p >> 16) & 0xff) | cg.put((p >> 8) & 0xff) | cb.put(p & 0xff);
            }
        }
    }
    return enoughColored(colored, static_cast<long>(r.w) * r.h);
}

void toXgray(const Canvas& c, uint8_t* dst, int dstStride, Rect r, uint32_t rmask, uint32_t gmask, uint32_t bmask) {
    r = r.intersected({0, 0, c.width(), c.height()});
    if (r.empty()) return;
    cairo_surface_flush(c.surface());
    const Channel cr(rmask), cg(gmask), cb(bmask);
    const bool bytes = byteChannels(cr, cg, cb);
    // Whole-byte channels: the gray in every one of them.
    const uint32_t spread = (1u << cr.shift) | (1u << cg.shift) | (1u << cb.shift);
    for (int y = r.y; y < r.y + r.h; ++y) {
        const uint32_t* src = canvasRow(c, y);
        auto* out = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * dstStride);
        for (int x = r.x; x < r.x + r.w; ++x) {
            const uint32_t p = src[x];
            const uint32_t g = (((p >> 16) & 0xff) * 77 + ((p >> 8) & 0xff) * 150 + (p & 0xff) * 29) >> 8;  // as toGray
            out[x] = bytes ? g * spread : cr.put(g) | cg.put(g) | cb.put(g);
        }
    }
}

void packRgb(const uint32_t* rgb24, int stride, Rect r, uint8_t* out) {
    for (int y = r.y; y < r.y + r.h; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(rgb24) + static_cast<size_t>(y) * stride);
        for (int x = r.x; x < r.x + r.w; ++x) {
            const uint32_t p = row[x];
            *out++ = static_cast<uint8_t>(p >> 16);
            *out++ = static_cast<uint8_t>(p >> 8);
            *out++ = static_cast<uint8_t>(p);
        }
    }
}

bool fbColorAllowed(const FbWriteState& s) { return s.foreground && s.unobscured && !s.keyboardUp; }

bool fbWritesRegion(bool colored, bool gateAllowed) { return colored && gateAllowed; }

bool usableMasks(uint32_t rmask, uint32_t gmask, uint32_t bmask) {
    for (uint32_t m : {rmask, gmask, bmask}) {
        if (!m) return false;
        const uint32_t bits = m >> std::countr_zero(m);
        if ((bits & (bits + 1)) != 0 || std::popcount(m) > 8) return false;  // contiguous, at most 8
    }
    return (rmask & gmask) == 0 && (rmask & bmask) == 0 && (gmask & bmask) == 0;
}

}  // namespace zk
