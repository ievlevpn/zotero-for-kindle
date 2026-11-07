#pragma once
// A screen that isn't 300 ppi (the Kindle 10th gen: 167). The app lays out in pixels at 300 ppi
// (metrics.h), so here it gets an imagined window of the screen's size at 300 ppi; what it
// presents is shrunk onto the screen and touches are stretched back. Everything keeps its size in
// millimetres; text is drawn at 300 ppi and shrunk, a little softer than drawn natively.

#include "platform/display.h"

#include <memory>

namespace zk {

class ScaledDisplay final : public Display {
public:
    // `scale`: screen pixels per app pixel (ppi / 300).
    ScaledDisplay(std::unique_ptr<Display> screen, double scale);

    int width() const override;
    int height() const override;
    Point origin() const override { return screen_->origin(); }  // on the screen, in its pixels
    std::string deviceName() const override { return screen_->deviceName(); }
    int ppi() const override { return 300; }
    void setOrientation(Orientation o) override { screen_->setOrientation(o); }
    void present(const Canvas&, Rect, Waveform) override;
    bool pollEvents(std::vector<InputEvent>&, int timeout_ms) override;
    bool systemInk() const override { return screen_->systemInk(); }
    bool setSystemInk(bool on) override { return screen_->setSystemInk(on); }

private:
    Point toApp(Point p) const;

    std::unique_ptr<Display> screen_;
    double scale_;
    Canvas shrunk_;  // the screen's pixels
};

}  // namespace zk
