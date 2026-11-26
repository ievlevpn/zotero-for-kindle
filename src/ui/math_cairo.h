#pragma once
// Plain-C Cairo 1.10 + Pango 1.26 + Fontconfig 2.8 backend for MicroTeX (no gtkmm/cairomm/pangomm).

#include <cairo.h>
#include <string>

#include "graphic/graphic.h"

namespace zk {

class MicroTeXGraphics final : public tex::Graphics2D {
public:
    explicit MicroTeXGraphics(cairo_t* cr);

    cairo_t* cr() const { return cr_; }

    void setColor(tex::color c) override;
    tex::color getColor() const override;
    void setStroke(const tex::Stroke& s) override;
    const tex::Stroke& getStroke() const override;
    void setStrokeWidth(float w) override;
    const tex::Font* getFont() const override;
    void setFont(const tex::Font* font) override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotate(float angle) override;
    void rotate(float angle, float px, float py) override;
    void reset() override;
    float sx() const override;
    float sy() const override;
    void drawChar(wchar_t c, float x, float y) override;
    void drawText(const std::wstring& t, float x, float y) override;
    void drawLine(float x1, float y1, float x2, float y2) override;
    void drawRect(float x, float y, float w, float h) override;
    void fillRect(float x, float y, float w, float h) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry) override;

private:
    void roundRectPath(float x, float y, float w, float h, float rx, float ry);

    cairo_t* cr_;
    cairo_matrix_t baseMatrix_{};
    tex::color color_ = tex::black;
    tex::Stroke stroke_;
    const tex::Font* font_ = nullptr;
    float sx_ = 1.f;
    float sy_ = 1.f;
};

}  // namespace zk
