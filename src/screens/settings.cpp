#include "screens/settings.h"

#include <algorithm>
#include <filesystem>

#include "app/settings_store.h"
#include "net/errors.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kW = 1236;
const int kIntervals[] = {0, 5, 10, 30};
const char* kIntervalLabels[] = {"Off", "5 min", "10 min", "30 min"};

// Section rows, top to bottom.
constexpr int kAccountY = 170, kWebDavY = 320, kIntervalY = 470, kHighlightsY = 640, kStorageY = 850, kFlashY = 1080,
              kAboutY = 1230;

std::string mb(int64_t bytes) {
    if (bytes < 1024 * 1024) return std::to_string((bytes + 1023) / 1024) + " KB";
    return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
}

}  // namespace

SettingsScreen::SettingsScreen(Config c, std::function<void(Nav&)> changeAccount)
    : config_(std::move(c)), changeAccount_(std::move(changeAccount)) {}

Rect SettingsScreen::accountRect() const { return {kW - kMargin - 240, kAccountY + 20, 240, 80}; }
Rect SettingsScreen::intervalRect(int i) const { return {kMargin + i * 279, kIntervalY + 60, 259, 80}; }
Rect SettingsScreen::toggleRect() const { return {kW - kMargin - 140, kHighlightsY, 140, 80}; }
Rect SettingsScreen::flashMinusRect() const { return {kW - kMargin - 460, kFlashY + 10, 120, 80}; }
Rect SettingsScreen::flashPlusRect() const { return {kW - kMargin - 120, kFlashY + 10, 120, 80}; }
Rect SettingsScreen::removeRect() const { return {kMargin, kStorageY + 80, kW - 2 * kMargin, 92}; }

void SettingsScreen::load(Library& lib) {
    interval_ = loadSettings(lib).syncIntervalMinutes;
    flashEvery_ = loadSettings(lib).flashEvery;
    allOffline_ = lib.allAnnotationsOffline();
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
}

void SettingsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, kW, 1547}, 255);
    drawHeader(cr, "Settings", true);
    const TextStyle label{kSans, 8.5, 500}, value{kSans, 7.5, 400, false, kSecondary};
    const int w = kW - 2 * kMargin;

    drawText(cr, "Zotero account", label, {kMargin, kAccountY, w, 50}, 1);
    drawText(cr, "User ID " + std::to_string(config_.userId), value, {kMargin, kAccountY + 54, w - 260, 50}, 1);
    drawButton(cr, accountRect(), "Change…", false);
    hRule(cr, kWebDavY - 30, kMargin, kW - kMargin);

    drawText(cr, "WebDAV", label, {kMargin, kWebDavY, w, 50}, 1);
    drawText(cr, config_.webdavUrl.empty() ? "Not set" : hostOf(config_.webdavUrl), value, {kMargin, kWebDavY + 54, w, 50}, 1);
    hRule(cr, kIntervalY - 30, kMargin, kW - kMargin);

    drawText(cr, "Sync every", label, {kMargin, kIntervalY, w, 50}, 1);
    int active = 2;
    for (int i = 0; i < 4; ++i)
        if (kIntervals[i] == interval_) active = i;
    for (int i = 0; i < 4; ++i) {
        const Rect r = intervalRect(i);
        drawButton(cr, r, kIntervalLabels[i], i == active);
    }
    hRule(cr, kHighlightsY - 30, kMargin, kW - kMargin);

    drawText(cr, "Keep all highlights offline", label, {kMargin, kHighlightsY + 4, w - 180, 50}, 1);
    drawText(cr,
             allOffline_ ? "Every highlight is downloaded and searchable offline."
                         : "Off: a book's highlights download when you open it. On: all of them (about 15 minutes "
                           "the first time for a large library).",
             value, {kMargin, kHighlightsY + 60, w - 180, 120}, 3);
    drawToggle(cr, toggleRect(), allOffline_);
    hRule(cr, kStorageY - 30, kMargin, kW - kMargin);

    drawText(cr, "Downloaded files: " + mb(downloadedBytes_), label, {kMargin, kStorageY, w, 50}, 1);
    drawButton(cr, removeRect(), confirmRemove_ ? "Tap again to remove them" : "Remove downloaded files", false);
    hRule(cr, kAboutY - 30, kMargin, kW - kMargin);

    drawText(cr, "Full refresh every", label, {kMargin, kFlashY + 24, w - 420, 50}, 1);
    drawButton(cr, flashMinusRect(), "−", false);
    drawButton(cr, flashPlusRect(), "+", false);
    centredText(cr, std::to_string(flashEvery_) + (flashEvery_ == 1 ? " page" : " pages"), {kSans, 8, 400},
                {flashMinusRect().x + 120, kFlashY + 10, flashPlusRect().x - flashMinusRect().x - 120, 80});
    hRule(cr, kAboutY - 30, kMargin, kW - kMargin);

    drawText(cr, "About", label, {kMargin, kAboutY, w, 50}, 1);
    drawText(cr, "Zotero for Kindle 0.3 · open source (AGPL-3.0)", value, {kMargin, kAboutY + 54, w, 50}, 1);
}

void SettingsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (accountRect().contains(g.pos)) {
        if (changeAccount_) changeAccount_(nav);
        return;
    }
    for (int i = 0; i < 4; ++i)
        if (intervalRect(i).contains(g.pos)) {
            AppSettings s = loadSettings(nav.library());
            s.syncIntervalMinutes = kIntervals[i];
            saveSettings(nav.library(), s);
            interval_ = kIntervals[i];
            nav.sync().setIntervalMinutes(interval_);
            nav.damage({0, kIntervalY, kW, 160}, Waveform::Ui);
            return;
        }
    if (flashMinusRect().contains(g.pos) || flashPlusRect().contains(g.pos)) {
        AppSettings st = loadSettings(nav.library());
        st.flashEvery = std::clamp(st.flashEvery + (flashPlusRect().contains(g.pos) ? 1 : -1), 1, 12);
        saveSettings(nav.library(), st);
        flashEvery_ = st.flashEvery;
        nav.damage({0, kFlashY, kW, 120}, Waveform::Ui);
        return;
    }
    if (toggleRect().contains(g.pos)) {
        allOffline_ = !allOffline_;
        nav.sync().setAllHighlightsOffline(allOffline_);
        nav.damage({0, kHighlightsY, kW, 200}, Waveform::Ui);
        return;
    }
    if (removeRect().contains(g.pos)) {
        if (!confirmRemove_) {
            confirmRemove_ = true;
        } else {
            confirmRemove_ = false;
            nav.sync().removeDownloads();
        }
        nav.damage({0, kStorageY, kW, 200}, Waveform::Ui);
        return;
    }
    if (confirmRemove_) {
        confirmRemove_ = false;
        nav.damage({0, kStorageY, kW, 200}, Waveform::Ui);
    }
}

void SettingsScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished && e.kind != SyncEvent::Kind::DownloadFinished) return;
    load(nav.library());
    nav.damage({0, kStorageY, kW, 200}, Waveform::Quality);
}

}  // namespace zk
