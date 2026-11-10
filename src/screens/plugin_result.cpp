#include "screens/plugin_result.h"

#include <algorithm>

#include "screens/screen.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

const TextStyle kLabel{kSans, 7, 500, false, kSecondary};
const TextStyle kBody{kSerif, 8.5, 400};
constexpr int kTextTop = 88, kGap = 30, kButtonH = 90, kBottom = 40, kButtonGap = 24;

int maxSheetH(Rect area) { return area.h * 11 / 20; }

}  // namespace

PluginResult::PluginResult(PluginEntries& entries, PluginAction action, std::string kind, nlohmann::ordered_json target, Rect area)
    : entries_(&entries), action_(std::move(action)), kind_(std::move(kind)), target_(std::move(target)), area_(area) {
    run_ = entries_->run(action_, kind_, target_);
}

PluginResult::PluginResult(std::string label, std::string text, Rect area)
    : area_(area), label_(std::move(label)), shown_(true), text_(std::move(text)) {
    layout();
}

PluginResult::Status PluginResult::status() const {
    if (closed_) return Status::Closed;
    return shown_ ? Status::Shown : Status::Running;
}

std::string PluginResult::label() const {
    return label_.empty() ? action_.pluginName + " · " + action_.title : label_;
}

std::string PluginResult::body() const { return running() ? "Working…" : text_; }

void PluginResult::layout() {
    const int w = area_.w - 2 * kMargin;
    const std::string b = body();
    tops_ = lineTops(b.empty() ? " " : b, kBody, w);
    const int textH = tops_.back();
    const int chrome = kTextTop + kGap + kButtonH + kBottom;
    const int room = std::max(tops_.size() > 1 ? tops_[1] : 50, maxSheetH(area_) - chrome);
    h_ = chrome + std::min(textH, room);
    starts_.assign(1, 0);
    const int lines = static_cast<int>(tops_.size()) - 1;
    for (int i = 0; i < lines; ++i)
        if (tops_[i + 1] - tops_[starts_.back()] > room) starts_.push_back(i);
    page_ = std::min(page_, pages() - 1);
}

Rect PluginResult::rect() const {
    if (!shown_ || closed_) return {};
    return {area_.x, area_.y + area_.h - h_, area_.w, h_};
}

Rect PluginResult::closeRect() const { return buttonRect(-1); }

Rect PluginResult::buttonRect(int i) const {
    const Rect r = rect();
    if (r.empty()) return {};
    const int shownButtons = running() ? 0 : static_cast<int>(buttons_.size());
    if (i >= shownButtons) return {};
    const int n = 1 + shownButtons, w = area_.w - 2 * kMargin;
    const int bw = std::min(380, (w - (n - 1) * kButtonGap) / n);
    return {r.x + kMargin + (i + 1) * (bw + kButtonGap), r.y + r.h - kBottom - kButtonH, bw, kButtonH};
}

void PluginResult::close(Nav& nav) {
    stop();
    if (shown_) nav.damage(rect(), Waveform::Quality);  // what is under it, without ghosts
    closed_ = true;
}

void PluginResult::stop() {
    if (run_ && entries_) entries_->stop(run_);
    run_ = 0;
}

PluginResult::Status PluginResult::tick(Nav& nav) {
    if (closed_ || !run_ || !entries_) return status();
    auto out = entries_->take(run_);
    if (!out) return status();
    run_ = 0;
    if (out->none) {
        close(nav);
        return status();
    }
    const Rect before = rect();
    text_ = out->text;
    buttons_ = out->error ? decltype(buttons_){} : out->buttons;
    if (buttons_.size() > 2) buttons_.resize(2);
    shown_ = true;
    page_ = 0;
    layout();
    const Rect after = rect();
    nav.damage(before.united(after), Waveform::Quality);
    return status();
}

PluginResult::Status PluginResult::onGesture(const Gesture& g, Nav& nav) {
    if (!shown_ || closed_) return status();
    if (g.type == GestureType::Swipe) {
        const int np = std::clamp(page_ + (g.dir == Direction::Left || g.dir == Direction::Up ? 1 : -1), 0, pages() - 1);
        if (np != page_) {
            page_ = np;
            nav.damage(rect(), Waveform::Quality);
        }
        return status();
    }
    if (g.type != GestureType::Tap) return status();
    if (closeRect().contains(g.pos)) {
        close(nav);
        return status();
    }
    for (int i = 0; i < static_cast<int>(buttons_.size()); ++i)
        if (buttonRect(i).contains(g.pos) && entries_) {
            const nlohmann::json again = {{"action", buttons_[i].first}, {"text", text_}};
            const Rect before = rect();
            run_ = entries_->run(action_, kind_, target_, again);
            page_ = 0;
            layout();
            nav.damage(before.united(rect()), Waveform::Quality);
            return status();
        }
    return status();
}

void PluginResult::draw(cairo_t* cr) const {
    const Rect r = rect();
    if (r.empty()) return;
    fillRect(cr, r, 255);
    fillRect(cr, {r.x, r.y, r.w, 3}, kInk);
    const int w = r.w - 2 * kMargin;
    const int indicatorW = pages() > 1 ? 200 : 0;
    drawText(cr, label(), kLabel, {r.x + kMargin, r.y + 34, w - indicatorW, 44}, 1);
    if (pages() > 1) drawPageIndicator(cr, {r.x + r.w - kMargin - indicatorW, r.y + 24, indicatorW, 64}, page_, pages());
    const int first = starts_[page_];
    const int end = page_ + 1 < pages() ? starts_[page_ + 1] : static_cast<int>(tops_.size()) - 1;
    const std::string b = body();
    drawTextLines(cr, b.empty() ? " " : b, running() ? TextStyle{kSerif, 8.5, 400, false, kSecondary} : kBody,
                  {r.x + kMargin, r.y + kTextTop, w, r.h - kTextTop - kGap - kButtonH - kBottom}, first, end - first);
    drawButton(cr, closeRect(), "Close", false);
    for (int i = 0; i < static_cast<int>(buttons_.size()); ++i)
        if (const Rect br = buttonRect(i); !br.empty()) drawButton(cr, br, buttons_[i].second, false);
}

}  // namespace zk
