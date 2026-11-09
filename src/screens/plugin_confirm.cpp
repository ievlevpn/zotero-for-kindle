#include "screens/plugin_confirm.h"

#include <algorithm>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kPad = 50;       // inside the box
constexpr int kButtonH = 90;
constexpr int kGap = 40;       // between and around the buttons
constexpr int kListGap = 26;   // between the question and the list
constexpr int kBulletW = 44;

const TextStyle kQuestion{kSans, 9, 500};
const TextStyle kListItem{kSerif, 8.5, 400};
const TextStyle kMore{kSans, 7.5, 400, false, kSecondary};
const TextStyle kBullet{kSans, 9, 700};

int listLineH() { return measureText("Ag", kListItem, 1000, 1).height + 18; }

}  // namespace

PluginConfirm::PluginConfirm(std::string pluginName, std::vector<std::string> lines, std::string action)
    : pluginName_(std::move(pluginName)), lines_(std::move(lines)), action_(std::move(action)) {
    if (lines_.empty()) lines_.push_back(pluginName_ + " wants to make a change.");
    if (action_.empty()) action_ = "Allow";
}

PluginConfirm::Layout PluginConfirm::layout() const {
    const Metrics& m = metrics();
    Layout l;
    l.box.x = kMargin;
    l.box.w = m.width - 2 * kMargin;
    const int textW = l.box.w - 2 * kPad;
    l.questionH = measureText(lines_[0], kQuestion, textW, 4).height;
    const int items = static_cast<int>(lines_.size()) - 1;
    const int fixed = kPad + l.questionH + (items > 0 ? kListGap : 0) + kPad + kButtonH + kGap;
    const int room = std::max(0, m.height - 2 * kMargin - fixed);  // for the list
    const int moreH = listLineH();
    int used = 0;
    for (int i = 0; i < items; ++i) {
        const int h = itemH(i);
        const bool last = i == items - 1;
        if (used + h + (last ? 0 : moreH) > room) {  // no room for it and what follows: "and N more"
            l.more = true;
            break;
        }
        used += h;
        l.listed = i + 1;
    }
    l.listH = used + (l.more ? moreH : 0);
    l.box.h = fixed + l.listH;
    l.box.y = std::max(kMargin, (m.height - l.box.h) / 3);
    return l;
}

int PluginConfirm::itemH(int i) const {
    const int textW = metrics().width - 2 * kMargin - 2 * kPad - kBulletW;
    return measureText(lines_[i + 1], kListItem, textW, 2).height + 18;
}

Rect PluginConfirm::rect() const { return layout().box; }

// Cancel on the left (the reader's page-back side), the action on the right.
Rect PluginConfirm::noRect() const {
    const Rect b = rect();
    const int w = (b.w - 3 * kGap) / 2;
    return {b.x + kGap, b.y + b.h - kGap - kButtonH, w, kButtonH};
}

Rect PluginConfirm::yesRect() const {
    const Rect no = noRect();
    return {no.x + no.w + kGap, no.y, no.w, kButtonH};
}

std::optional<bool> PluginConfirm::onTap(Point p) const {
    if (yesRect().contains(p)) return true;
    if (noRect().contains(p)) return false;
    return std::nullopt;
}

void PluginConfirm::draw(cairo_t* cr) const {
    const Layout l = layout();
    const Rect b = l.box;
    cairo_save(cr);
    roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, 18);
    gray(cr, 255);
    cairo_fill_preserve(cr);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    const int textW = b.w - 2 * kPad;
    drawText(cr, lines_[0], kQuestion, {b.x + kPad, b.y + kPad, textW, l.questionH + 4}, 4);
    int y = b.y + kPad + l.questionH + kListGap;
    for (int i = 0; i < l.listed; ++i) {
        const int h = itemH(i);
        drawText(cr, "•", kBullet, {b.x + kPad, y, kBulletW, h}, 1);
        drawText(cr, lines_[i + 1], kListItem, {b.x + kPad + kBulletW, y, textW - kBulletW, h}, 2);
        y += h;
    }
    if (l.more) {
        const int rest = static_cast<int>(lines_.size()) - 1 - l.listed;
        drawText(cr, "and " + std::to_string(rest) + " more", kMore, {b.x + kPad + kBulletW, y + 6, textW - kBulletW, listLineH()}, 1);
    }
    drawButton(cr, yesRect(), action_, true);
    drawButton(cr, noRect(), "Cancel", false);
    cairo_restore(cr);
}

}  // namespace zk
