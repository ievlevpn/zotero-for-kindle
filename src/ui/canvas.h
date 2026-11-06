#pragma once

#include <cstdint>

#include <cairo.h>

#include "core/geometry.h"

namespace zk {

// The app's drawing surface: an RGB24 cairo image the size of the screen.
// Converted to 8-bit gray when presented on the e-ink display.
class Canvas {
public:
    Canvas(int w, int h);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    cairo_t* cr() { return cr_; }
    cairo_surface_t* surface() const { return surface_; }
    int width() const { return w_; }
    int height() const { return h_; }

    // Writes luma (r*77 + g*150 + b*29) >> 8 for `r` into an 8-bit buffer whose
    // pixel (0,0) corresponds to the canvas origin.
    void toGray(uint8_t* dst, int dst_stride, Rect r) const;

private:
    int w_, h_;
    cairo_surface_t* surface_;
    cairo_t* cr_;
};

}  // namespace zk
