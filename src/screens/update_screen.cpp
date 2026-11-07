#include "screens/update_screen.h"

#include "platform/power.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <cstdio>

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }
constexpr int kTitleY = kHeaderH + 40, kSubY = kTitleY + 90, kBarY = kSubY + 70, kNotesY = kBarY + 70;

std::string mb(int64_t bytes, bool decimal) {
    char buf[32];
    if (decimal) std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(bytes) / (1024 * 1024));
    else std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>((bytes + 512 * 1024) / (1024 * 1024)));
    return buf;
}

int percent(const Updater::Status& s) { return s.total > 0 ? static_cast<int>(s.done * 100 / s.total) : 0; }

}  // namespace

UpdateScreen::UpdateScreen(Updater& u, std::function<void()> stopSync, KeepAwake* keepAwake,
                           std::function<void()> resumeSync)
    : updater_(u), stopSync_(std::move(stopSync)), resumeSync_(std::move(resumeSync)), keepAwake_(keepAwake),
      shown_(u.status()) {}

UpdateScreen::~UpdateScreen() = default;

Rect UpdateScreen::primaryRect() {
    const int w = (W() - 2 * kMargin - 40) / 2;
    return {kMargin, H() - kMargin - 100, w, 100};
}

Rect UpdateScreen::secondaryRect() {
    const Rect p = primaryRect();
    return {p.x + p.w + 40, p.y, p.w, p.h};
}

void UpdateScreen::onShow(Nav&) { shown_ = updater_.status(); }

void UpdateScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    const Updater::Status s = updater_.status();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Software update", true);
    const TextStyle title{kSans, 11, 700}, sub{kSans, 7.5, 400, false, kSecondary}, body{kSerif, 8, 400};
    const int w = W() - 2 * kMargin;
    const std::string version = s.release.version.str();
    std::string heading, line, text = s.release.notes;
    switch (s.phase) {
        case Phase::Downloading:
            heading = "Downloading… " + mb(s.done, true) + " of " + mb(s.total ? s.total : s.release.zipSize, true) + " MB";
            break;
        case Phase::Downloaded:
        case Phase::Installing:
        case Phase::ReadyToRestart:
            heading = "Installing…";
            line = "Zotero restarts when it's done.";
            text.clear();
            break;
        case Phase::Failed:
            heading = s.mustRestart ? "The update didn't finish" : "Couldn't update";
            line.clear();
            text = s.error.message;
            break;
        case Phase::NotInstallable:
            heading = "Version " + version + " is available";
            text = "This release can't be installed from the app. See github.com/ievlevpn/zotero-for-kindle.\n\n" +
                   s.release.notes;
            break;
        default:
            heading = "Version " + version + " is available";
            line = mb(s.release.zipSize, false) + " MB";
            break;
    }
    drawText(cr, heading, title, {kMargin, kTitleY, w, 80}, 1);
    if (!line.empty()) drawText(cr, line, sub, {kMargin, kSubY, w, 50}, 1);
    if (s.phase == Phase::Downloading) {
        const Rect bar{kMargin, kBarY, w, 14};
        fillRect(cr, bar, kRule);
        fillRect(cr, {bar.x, bar.y, static_cast<int>(static_cast<int64_t>(bar.w) * percent(s) / 100), bar.h}, kInk);
    }
    hRule(cr, kNotesY - 30, kMargin, W() - kMargin);
    const int notesBottom = primaryRect().y - 50;
    if (!text.empty()) drawText(cr, text, body, {kMargin, kNotesY, w, notesBottom - kNotesY}, 1000);

    switch (s.phase) {
        case Phase::Available:
            if (updater_.canInstall()) drawButton(cr, primaryRect(), "Update", true);
            else drawText(cr, "Updates install on the Kindle.", sub, {primaryRect().x, primaryRect().y + 30, primaryRect().w, 50}, 1);
            drawButton(cr, secondaryRect(), "Not now", false);
            break;
        case Phase::NotInstallable:
            drawButton(cr, secondaryRect(), "Not now", false);
            break;
        case Phase::Downloading:
            drawButton(cr, secondaryRect(), "Cancel", false);
            break;
        case Phase::Failed:
            if (s.mustRestart) {
                drawButton(cr, primaryRect(), "Restart", true);
            } else {
                if (updater_.canInstall() && s.release.zipSize > 0) drawButton(cr, primaryRect(), "Try again", true);
                drawButton(cr, secondaryRect(), "Not now", false);
            }
            break;
        default:
            break;
    }
}

void UpdateScreen::leave(Nav& nav) {
    if (updater_.status().phase == Phase::Downloading) updater_.cancel();
    nav.pop();
}

void UpdateScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    const Updater::Status s = updater_.status();
    const bool installing = s.phase == Phase::Downloaded || s.phase == Phase::Installing ||
                            s.phase == Phase::ReadyToRestart || s.mustRestart;
    if (headerBackRect().contains(g.pos)) {
        if (!installing) leave(nav);
        return;
    }
    if (primaryRect().contains(g.pos)) {
        if (s.mustRestart) {
            nav.restart();
        } else if (s.phase == Phase::Available || s.phase == Phase::Failed) {
            updater_.download();
            nav.damage({0, 0, W(), H()}, Waveform::Ui);
        }
        return;
    }
    if (secondaryRect().contains(g.pos) && !installing) leave(nav);
}

void UpdateScreen::onTick(Nav& nav) {
    const Updater::Status s = updater_.status();
    const bool busy = s.phase == Phase::Downloading || s.phase == Phase::Downloaded || s.phase == Phase::Installing ||
                      s.phase == Phase::ReadyToRestart;
    if (busy && !awake_ && keepAwake_) awake_ = std::make_unique<KeepAwakeScope>(*keepAwake_);
    if (!busy) awake_.reset();
    if (s.phase == Phase::Downloading) installRequested_ = syncResumed_ = false;  // "Try again"
    // An install that failed without changing anything: sync again. (After a failed swap the
    // app restarts instead, and the launcher puts the old version back.)
    if (s.phase == Phase::Failed && installRequested_ && !s.mustRestart && !syncResumed_) {
        syncResumed_ = true;
        if (resumeSync_) resumeSync_();
    }
    if (s.phase == Phase::Downloaded && !installRequested_) {
        installRequested_ = true;
        if (stopSync_) stopSync_();
        updater_.install();
    }
    if (s.phase == Phase::ReadyToRestart && !restartRequested_) {
        restartRequested_ = true;
        nav.restart();
    }
    if (s.serial == shown_.serial) return;
    const bool progressOnly = s.phase == shown_.phase && s.phase == Phase::Downloading;
    if (progressOnly && percent(s) == shownPercent_) return;
    shownPercent_ = percent(s);
    if (progressOnly) nav.damage({0, kTitleY, W(), kBarY + 30 - kTitleY}, Waveform::Fast);
    else nav.damage({0, 0, W(), H()}, Waveform::Ui);
    shown_ = s;
}

}  // namespace zk
