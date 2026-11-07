#include "screens/goto_page.h"

#include <cmath>
#include <cstdlib>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
const char* const kKeys[4][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"%", "0", "⌫"}};
constexpr int kKeyW = 300, kKeyH = 150, kGap = 24, kFieldY = kHeaderH + 40, kPadY = kFieldY + 260;

}  // namespace

GoToPageScreen::GoToPageScreen(int pageCount, int currentPage, std::function<void(int)> go)
    : pageCount_(pageCount), current_(currentPage), go_(std::move(go)) {}

Rect GoToPageScreen::keyRect(const std::string& key) {
    const int left = (W() - 3 * kKeyW - 2 * kGap) / 2;
    if (key == "Go") return {left, kPadY + 4 * (kKeyH + kGap), 3 * kKeyW + 2 * kGap, 110};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            if (key == kKeys[r][c]) return {left + c * (kKeyW + kGap), kPadY + r * (kKeyH + kGap), kKeyW, kKeyH};
    return {};
}

void GoToPageScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), metrics().height}, 255);
    drawHeader(cr, "Go to", true);
    const int left = (W() - 3 * kKeyW - 2 * kGap) / 2, width = 3 * kKeyW + 2 * kGap;
    // The field.
    const Rect field{left, kFieldY, width, 120};
    roundedRect(cr, field.x + 1.5, field.y + 1.5, field.w - 3, field.h - 3, 12);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    const bool empty = entry_.empty();
    drawText(cr, empty ? "Page " + std::to_string(current_ + 1) : entry_, {kSans, 13, empty ? 400 : 700, false, empty ? kTertiary : kInk},
             {field.x + 30, field.y + 22, field.w - 60, 90}, 1);
    drawText(cr, error_.empty() ? "1–" + std::to_string(pageCount_) + ", or a percentage" : error_,
             {kSans, 7.5, error_.empty() ? 400 : 700, false, error_.empty() ? kSecondary : kInk},
             {left, field.y + field.h + 24, width, 50}, 1);
    for (const auto& row : kKeys)
        for (const char* k : row) drawButton(cr, keyRect(k), k, false);
    drawButton(cr, keyRect("Go"), "Go", true);
}

void GoToPageScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    for (const char* k : {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "%", "⌫", "Go"})
        if (keyRect(k).contains(g.pos)) return press(k, nav);
}

void GoToPageScreen::press(const std::string& key, Nav& nav) {
    error_.clear();
    const bool percent = !entry_.empty() && entry_.back() == '%';
    if (key == "⌫") {
        if (!entry_.empty()) entry_.pop_back();
    } else if (key == "%") {
        if (!entry_.empty() && !percent) entry_ += "%";
    } else if (key == "Go") {
        if (entry_.empty()) return;
        const long n = std::strtol(entry_.c_str(), nullptr, 10);
        int page = -1;
        if (percent) {
            if (n <= 100) page = static_cast<int>(std::lround(n / 100.0 * std::max(0, pageCount_ - 1)));
            else error_ = "Up to 100%.";
        } else if (n >= 1 && n <= pageCount_) {
            page = static_cast<int>(n) - 1;
        } else {
            error_ = "There are " + std::to_string(pageCount_) + " pages.";
        }
        if (page >= 0) {
            nav.pop();
            if (go_) go_(page);
            return;
        }
    } else if (!percent && entry_.size() < 6) {
        entry_ += key;
    }
    nav.damage({0, kFieldY, W(), kPadY - kFieldY}, Waveform::Fast);
}

}  // namespace zk
