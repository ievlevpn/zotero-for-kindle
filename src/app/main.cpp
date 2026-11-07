#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "platform/display.h"
#include "screens/home_demo.h"
#include "ui/gesture.h"
#include "ui/text.h"

using namespace zk;

namespace {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Renders the Home demo once to a PNG and exits (screenshots, golden reviews).
int screenshot(const char* path) {
    Canvas c(1236, 1547);
    drawHomeDemo(c, HomeDemoData{});
    return cairo_surface_write_to_png(c.surface(), path) == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    log(LogLevel::Info, "zotero starting (backend=%s)", ZK_BACKEND);
    addFontDir(fontDir());
    if (argc > 2 && !std::strcmp(argv[1], "--screenshot")) return screenshot(argv[2]);
    if (argc > 1 && !std::strcmp(argv[1], "--version")) return 0;

    std::unique_ptr<Display> display = makeDisplay();
    Canvas canvas(display->width(), display->height());
    HomeDemoData data;
    drawHomeDemo(canvas, data);
    display->present(canvas, {0, 0, canvas.width(), canvas.height()}, Waveform::Full);

    GestureRecognizer gestures;
    std::vector<InputEvent> events;
    const int tabs = (int)data.tabs.size();
    const Rect header{0, 0, canvas.width(), 140};
    for (;;) {
        events.clear();
        if (!display->pollEvents(events, 50)) break;
        std::vector<Gesture> out;
        for (const InputEvent& e : events) {
            if (e.kind != InputEvent::Kind::Touch) continue;
            auto g = gestures.feed(e.touch);
            out.insert(out.end(), g.begin(), g.end());
        }
        auto ticked = gestures.tick(nowMs());
        out.insert(out.end(), ticked.begin(), ticked.end());

        for (const Gesture& g : out) {
            if (g.type == GestureType::LongPress && header.contains(g.pos)) {
                log(LogLevel::Info, "long press on header: exit");
                return 0;
            }
            if (g.type != GestureType::Tap) continue;
            log(LogLevel::Info, "Tap at (%d,%d)", g.pos.x, g.pos.y);
            for (int i = 0; i < tabs; ++i) {
                Rect r = homeDemoTabRect(canvas, i, tabs);
                if (!r.contains(g.pos) || i == data.activeTab) continue;
                // Instant feedback: invert the tapped tab (fast waveform), then draw the new state.
                int64_t t0 = nowMs();
                cairo_set_source_rgb(canvas.cr(), 0, 0, 0);
                cairo_rectangle(canvas.cr(), r.x, r.y + 8, r.w, r.h - 8);
                cairo_fill(canvas.cr());
                display->present(canvas, r, Waveform::Fast);
                log(LogLevel::Info, "tab %d feedback presented in %lld ms", i, (long long)(nowMs() - t0));
                data.activeTab = i;
                drawHomeDemo(canvas, data);
                Rect bar = homeDemoTabRect(canvas, 0, tabs).united(homeDemoTabRect(canvas, tabs - 1, tabs));
                display->present(canvas, bar, Waveform::Quality);
            }
        }
    }
    return 0;
}
