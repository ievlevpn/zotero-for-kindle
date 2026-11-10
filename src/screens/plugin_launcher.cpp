#include "screens/plugin_launcher.h"

#include <algorithm>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }
constexpr int kListRowH = 160;
constexpr int kTop = kHeaderH + 10;

}  // namespace

int PluginLauncherScreen::perPage() const { return std::max(1, (H() - kTop - kPageIndicatorH) / kListRowH); }

void PluginLauncherScreen::load(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    version_ = e ? e->version() : 0;
    rows_ = e ? e->mainScreens() : std::vector<PluginLaunch>{};
    pager_ = Pager(static_cast<int64_t>(rows_.size()), perPage());
}

void PluginLauncherScreen::onShow(Nav& nav) { load(nav); }

void PluginLauncherScreen::onTick(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    if (!e || e->version() == version_) return;
    const int page = pager_.page();
    load(nav);
    while (pager_.page() < page && pager_.next()) {
    }
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Ui);
}

std::vector<Rect> PluginLauncherScreen::rowRects() const {
    std::vector<Rect> out;
    for (int i = 0; i < perPage() && pager_.offset() + i < static_cast<int64_t>(rows_.size()); ++i)
        out.push_back({kMargin, kTop + i * kListRowH, W() - 2 * kMargin, kListRowH});
    return out;
}

void PluginLauncherScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Plugins", true);
    const TextStyle title{kSans, 9.5, 500}, quiet{kSans, 7, 400, false, kSecondary}, badge{kSans, 7, 500};
    if (rows_.empty()) {
        drawText(cr, "No plugin has a screen to open.", quiet, {kMargin, kTop + 20, W() - 2 * kMargin, 120}, 2);
        return;
    }
    const auto rects = rowRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect r = rects[i];
        const PluginLaunch& l = rows_[static_cast<size_t>(pager_.offset()) + i];
        int textW = r.w;
        if (!l.badge.empty()) {
            const int bw = std::min(measureText(l.badge, badge, r.w / 3, 1).width + 40, r.w / 3);
            const Rect b{r.x + r.w - bw, r.y + (r.h - 64) / 2, bw, 64};
            roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, 32);
            gray(cr, kInk);
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
            centredText(cr, l.badge, badge, b);
            textW -= bw + 30;
        }
        drawText(cr, l.title, title, {r.x, r.y + 26, textW, 60}, 1);
        drawText(cr, l.pluginName, quiet, {r.x, r.y + 92, textW, 46}, 1);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
    drawPageIndicator(cr, {0, H() - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void PluginLauncherScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    const auto rects = rowRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos)) {
            const PluginLaunch l = rows_[static_cast<size_t>(pager_.offset()) + i];
            if (PluginEntries* e = nav.pluginEntries()) e->openScreen(l.pluginId, l.screenId);
            return;
        }
}

}  // namespace zk
