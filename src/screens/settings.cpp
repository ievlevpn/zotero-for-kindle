#include "screens/settings.h"

#include <algorithm>
#include <filesystem>

#include "app/settings_store.h"
#include "screens/read_tag.h"
#include "net/errors.h"
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
const int kIntervals[] = {0, 5, 10, 30};
const char* kIntervalLabels[] = {"Off", "5 min", "10 min", "30 min"};

// Section rows, top to bottom.
// Section rows, top to bottom; `rule`: the line above a section, this far above it. A window
// shorter than the PW5's (the KT4's, 1337 at 300 ppi) takes the compact spacing.
struct Rows {
    int account, webDav, readTag, interval, highlights, storage, flash, about, rule;
};
constexpr Rows kRows{170, 320, 470, 620, 790, 1000, 1230, 1380, 30};
constexpr Rows kCompactRows{150, 290, 410, 540, 710, 870, 1072, 1185, 20};
const Rows& rows() { return H() < 1500 ? kCompactRows : kRows; }

std::string mb(int64_t bytes) {
    if (bytes < 1024 * 1024) return std::to_string((bytes + 1023) / 1024) + " KB";
    return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
}

}  // namespace

SettingsScreen::SettingsScreen(Config c, std::function<void(Nav&)> changeAccount, Updater* updater,
                               std::function<void(Nav&)> openUpdate,
                               std::function<std::optional<std::string>()> pendingUpdate)
    : config_(std::move(c)), changeAccount_(std::move(changeAccount)), updater_(updater),
      openUpdate_(std::move(openUpdate)), pendingUpdate_(std::move(pendingUpdate)) {}

Rect SettingsScreen::accountRect() const { return {W() - kMargin - 240, rows().account + 20, 240, 80}; }
Rect SettingsScreen::readTagRect() const { return {W() - kMargin - 240, rows().readTag + 20, 240, 80}; }
Rect SettingsScreen::intervalRect(int i) const {
    const int w = std::min(259, (W() - 2 * kMargin - 3 * 20) / 4);  // four in a row on a narrow window
    return {kMargin + i * (w + 20), rows().interval + 60, w, 80};
}
Rect SettingsScreen::aboutTextRect() const { return {kMargin, rows().about + 54, W() - 2 * kMargin - 460, 96}; }
Rect SettingsScreen::toggleRect() const { return {W() - kMargin - 140, rows().highlights, 140, 80}; }
Rect SettingsScreen::flashMinusRect() const { return {W() - kMargin - 460, rows().flash + 10, 120, 80}; }
Rect SettingsScreen::flashPlusRect() const { return {W() - kMargin - 120, rows().flash + 10, 120, 80}; }
Rect SettingsScreen::updateRect() const { return {W() - kMargin - 420, rows().about + 20, 420, 80}; }

std::string SettingsScreen::updateLine() const {
    if (!updateResult_.empty()) return updateResult_;
    if (pendingUpdate_)
        if (const auto v = pendingUpdate_()) return "Update available: " + *v;
    return "Version " + (updater_ ? updater_->current() : appVersion()).str() + " · open source (AGPL-3.0)";
}

Rect SettingsScreen::removeRect() const { return {kMargin, rows().storage + 80, W() - 2 * kMargin, 92}; }

void SettingsScreen::load(Library& lib) {
    interval_ = loadSettings(lib).syncIntervalMinutes;
    flashEvery_ = loadSettings(lib).flashEvery;
    allOffline_ = lib.allAnnotationsOffline();
    readTag_ = lib.readTag();
    downloadedBytes_ = 0;
    for (const auto& a : lib.downloadedAttachments()) {
        std::error_code ec;
        const auto n = std::filesystem::file_size(a.localPath, ec);
        if (!ec) downloadedBytes_ += static_cast<int64_t>(n);
    }
}

void SettingsScreen::onShow(Nav& nav) {
    load(nav.library());
    confirmRemove_ = false;
    // A check already running (the Home banner's): its answer opens the update screen as ours would.
    if (updater_ && updater_->status().phase == Updater::Status::Phase::Checking) checking_ = true;
}

void SettingsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Settings", true);
    const TextStyle label{kSans, 8.5, 500}, value{kSans, 7.5, 400, false, kSecondary};
    const int w = W() - 2 * kMargin;

    drawText(cr, "Zotero account", label, {kMargin, rows().account, w, 50}, 1);
    drawText(cr, "User ID " + std::to_string(config_.userId), value, {kMargin, rows().account + 54, w - 260, 50}, 1);
    drawButton(cr, accountRect(), "Change…", false);
    hRule(cr, rows().webDav - rows().rule, kMargin, W() - kMargin);

    drawText(cr, "WebDAV", label, {kMargin, rows().webDav, w, 50}, 1);
    drawText(cr, config_.webdavUrl.empty() ? "Not used: files come from Zotero storage" : hostOf(config_.webdavUrl), value, {kMargin, rows().webDav + 54, w, 50}, 1);
    hRule(cr, rows().readTag - rows().rule, kMargin, W() - kMargin);

    drawText(cr, "Read tag", label, {kMargin, rows().readTag, w - 260, 50}, 1);
    drawText(cr, "Items tagged “" + readTag_ + "” in Zotero show as read", value,
             {kMargin, rows().readTag + 54, w - 260, 50}, 1);
    drawButton(cr, readTagRect(), "Change…", false);
    hRule(cr, rows().interval - rows().rule, kMargin, W() - kMargin);

    drawText(cr, "Sync every", label, {kMargin, rows().interval, w, 50}, 1);
    int active = 2;
    for (int i = 0; i < 4; ++i)
        if (kIntervals[i] == interval_) active = i;
    for (int i = 0; i < 4; ++i) {
        const Rect r = intervalRect(i);
        drawButton(cr, r, kIntervalLabels[i], i == active);
    }
    hRule(cr, rows().highlights - rows().rule, kMargin, W() - kMargin);

    drawText(cr, "Keep all highlights offline", label, {kMargin, rows().highlights + 4, w - 180, 50}, 1);
    drawText(cr,
             allOffline_ ? "Every highlight is downloaded and searchable offline."
                         : "Off: a book's highlights download when you open it. On: all of them (the first "
                           "time can take 15 minutes).",
             value, {kMargin, rows().highlights + 60, w - 180, 120}, 3);
    drawToggle(cr, toggleRect(), allOffline_);
    hRule(cr, rows().storage - rows().rule, kMargin, W() - kMargin);

    drawText(cr, "Downloaded files: " + mb(downloadedBytes_), label, {kMargin, rows().storage, w, 50}, 1);
    drawButton(cr, removeRect(), confirmRemove_ ? "Tap again to remove them" : "Remove downloaded files", false);
    hRule(cr, rows().flash - rows().rule, kMargin, W() - kMargin);

    drawText(cr, "Full refresh every", label, {kMargin, rows().flash + 24, w - 420, 50}, 1);
    drawButton(cr, flashMinusRect(), "−", false);
    drawButton(cr, flashPlusRect(), "+", false);
    centredText(cr, std::to_string(flashEvery_) + (flashEvery_ == 1 ? " page" : " pages"), {kSans, 8, 400},
                {flashMinusRect().x + 120, rows().flash + 10, flashPlusRect().x - flashMinusRect().x - 120, 80});
    hRule(cr, rows().about - rows().rule, kMargin, W() - kMargin);

    drawText(cr, "Zotero for Kindle", label, {kMargin, rows().about, w - 460, 50}, 1);
    drawText(cr, updateLine(), value, aboutTextRect(), 2);
    drawButton(cr, updateRect(), checking_ ? "Checking…" : "Check for updates", false);
}

void SettingsScreen::onTick(Nav& nav) {
    if (!checking_ || !updater_) return;
    using Phase = Updater::Status::Phase;
    const Updater::Status st = updater_->status();
    if (st.phase == Phase::Checking) return;
    checking_ = false;
    if (st.phase == Phase::UpToDate) {
        updateResult_ = "You have the latest version.";
    } else if (st.phase == Phase::Failed && !st.mustRestart) {
        updateResult_ = st.error.message;
    } else if (st.phase != Phase::Idle) {  // a newer release, or an update already under way
        if (openUpdate_) openUpdate_(nav);
    }
    nav.damage({0, rows().about - 10, W(), 200}, Waveform::Ui);
}

void SettingsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (updateRect().contains(g.pos)) {
        if (!updater_ || checking_) return;
        updateResult_.clear();
        updater_->reset();
        updater_->check();
        checking_ = true;
        nav.damage({0, rows().about - 10, W(), 200}, Waveform::Ui);
        return;
    }
    if (accountRect().contains(g.pos)) {
        if (changeAccount_) changeAccount_(nav);
        return;
    }
    if (readTagRect().contains(g.pos)) {
        nav.push(std::make_unique<ReadTagScreen>());
        return;
    }
    for (int i = 0; i < 4; ++i)
        if (intervalRect(i).contains(g.pos)) {
            AppSettings s = loadSettings(nav.library());
            s.syncIntervalMinutes = kIntervals[i];
            saveSettings(nav.library(), s);
            interval_ = kIntervals[i];
            nav.sync().setIntervalMinutes(interval_);
            nav.damage({0, rows().interval, W(), 160}, Waveform::Ui);
            return;
        }
    if (flashMinusRect().contains(g.pos) || flashPlusRect().contains(g.pos)) {
        AppSettings st = loadSettings(nav.library());
        st.flashEvery = std::clamp(st.flashEvery + (flashPlusRect().contains(g.pos) ? 1 : -1), 1, 12);
        saveSettings(nav.library(), st);
        flashEvery_ = st.flashEvery;
        nav.damage({0, rows().flash, W(), 120}, Waveform::Ui);
        return;
    }
    if (toggleRect().contains(g.pos)) {
        allOffline_ = !allOffline_;
        nav.sync().setAllHighlightsOffline(allOffline_);
        nav.damage({0, rows().highlights, W(), 200}, Waveform::Ui);
        return;
    }
    if (removeRect().contains(g.pos)) {
        if (!confirmRemove_) {
            confirmRemove_ = true;
        } else {
            confirmRemove_ = false;
            nav.sync().removeDownloads();
        }
        nav.damage({0, rows().storage, W(), 200}, Waveform::Ui);
        return;
    }
    if (confirmRemove_) {
        confirmRemove_ = false;
        nav.damage({0, rows().storage, W(), 200}, Waveform::Ui);
    }
}

void SettingsScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished && e.kind != SyncEvent::Kind::DownloadFinished) return;
    load(nav.library());
    nav.damage({0, rows().storage, W(), 200}, Waveform::Quality);
}

}  // namespace zk
