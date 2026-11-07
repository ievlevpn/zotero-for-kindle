#include "ui/canvas.h"

namespace zk {

Canvas::Canvas(int w, int h)
    : w_(w), h_(h), surface_(cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h)), cr_(cairo_create(surface_)) {}

void Canvas::resize(int w, int h) {
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
    w_ = w;
    h_ = h;
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cr_ = cairo_create(surface_);
    cairo_set_source_rgb(cr_, 1, 1, 1);
    cairo_paint(cr_);
}

Canvas::~Canvas() {
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
}

void Canvas::toGray(uint8_t* dst, int dst_stride, Rect r) const {
    r = r.intersected(Rect{0, 0, w_, h_});
    if (r.empty()) return;
    cairo_surface_flush(surface_);
    const uint8_t* data = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    for (int y = r.y; y < r.y + r.h; ++y) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(data + y * stride);
        uint8_t* out = dst + y * dst_stride;
        for (int x = r.x; x < r.x + r.w; ++x) {
            uint32_t p = src[x];
            uint32_t red = (p >> 16) & 0xff, green = (p >> 8) & 0xff, blue = p & 0xff;
            out[x] = static_cast<uint8_t>((red * 77 + green * 150 + blue * 29) >> 8);
        }
    }
}

}  // namespace zk
