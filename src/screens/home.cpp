#include "screens/home.h"

#include <algorithm>

#include <ctime>

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
constexpr int kCardY = 292, kCoverW = 290, kCoverH = 400;
constexpr int kRecentLabelY = kCardY + kCoverH + 80;
constexpr int kRowsY = kRecentLabelY + 56;
constexpr int kBannerH = 150;
constexpr int64_t kResyncAfterMs = 2 * 60'000;

std::string authorsOf(const std::string& byline) {
    const size_t dot = byline.find(" · ");
    return dot == std::string::npos ? byline : byline.substr(0, dot);
}

std::string openedAgo(int64_t openedMs, int64_t nowMs) {
    const int64_t days = (nowMs - openedMs) / (24 * 3600 * 1000LL);
    if (days < 1) return "opened today";
    if (days < 2) return "opened yesterday";
    if (days < 7) return "opened " + std::to_string(days) + " days ago";
    std::time_t t = static_cast<std::time_t>(openedMs / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "opened %Y-%m-%d", &tm);
    return buf;
}

}  // namespace

HomeScreen::HomeScreen(SyncStatus& status, HomeActions actions, std::function<int64_t()> wallMs)
    : status_(status), actions_(std::move(actions)), wallMs_(std::move(wallMs)) {}

// Top right: settings, sync, close.
Rect HomeScreen::closeRect() { return {W() - kMargin - 80, 34, 80, 96}; }
Rect HomeScreen::syncIndicatorRect() { return {W() - kMargin - 180, 34, 80, 96}; }
Rect HomeScreen::settingsRect() { return {W() - kMargin - 280, 34, 80, 96}; }
Rect HomeScreen::searchFieldRect() { return {kMargin, 150, W() - 2 * kMargin, 92}; }
Rect HomeScreen::cardRect() { return {kMargin, kCardY, W() - 2 * kMargin, kCoverH}; }

Rect HomeScreen::bannerRect() const { return {0, H() - kTabBarH - kBannerH, W(), kBannerH}; }

std::vector<Rect> HomeScreen::recentRowRects() const {
    std::vector<Rect> rows;
    const int bottom = status_.hasError() ? bannerRect().y : H() - kTabBarH;
    for (int i = 0, y = kRowsY; i < static_cast<int>(recent_.size()) && y + kRowH <= bottom; ++i, y += kRowH)
        rows.push_back({kMargin, y, W() - 2 * kMargin, kRowH});
    return rows;
}

void HomeScreen::reload(Library& lib) {
    ListQuery all;
    total_ = lib.countRows(all);
    card_.reset();
    cardOpened_ = false;
    cardHighlights_ = 0;
    if (auto last = lib.lastOpened()) {
        card_ = lib.rowFor(last->itemKey);
        cardOpened_ = card_.has_value();
        cardOpenedMs_ = last->openedMs;
    }
    if (!card_) {  // nothing opened yet: suggest the newest readable item
        ListQuery q;
        q.limit = 20;
        for (const auto& r : lib.rows(q))
            if (!r.badge.empty()) {
                card_ = r;
                break;
            }
    }
    if (card_ && !card_->attachmentKey.empty()) cardHighlights_ = lib.annotationsFor(card_->attachmentKey).size();
    // Recent: the books read last (after the one in the card), then the newest items if fewer —
    // as many as fit under the card (3 on the PW5).
    const size_t fit = static_cast<size_t>(std::max(1, (H() - kTabBarH - kRowsY) / kRowH));
    recent_.clear();
    auto seen = [&](const std::string& key) {
        if (card_ && card_->key == key) return true;
        for (const auto& r : recent_)
            if (r.key == key) return true;
        return false;
    };
    for (auto& r : lib.recentlyOpened(static_cast<int>(fit) + 1))
        if (recent_.size() < fit && !seen(r.key)) recent_.push_back(std::move(r));
    if (recent_.size() < fit) {
        ListQuery newest;
        newest.limit = static_cast<int>(2 * fit);
        for (auto& r : lib.rows(newest))
            if (recent_.size() < fit && !seen(r.key)) recent_.push_back(std::move(r));
    }
}

void HomeScreen::onShow(Nav& nav) {
    nowMs_ = nav.nowMs();
    reload(nav.library());
    if (!status_.syncing() && (status_.hasError() || status_.msSinceSync(nav.nowMs()) > kResyncAfterMs)) nav.sync().syncNow();
}

std::string HomeScreen::cardMeta() const {
    std::string meta = std::to_string(cardHighlights_) + (cardHighlights_ == 1 ? " highlight" : " highlights");
    if (cardOpened_) {
        if (card_->progress > 0) meta = std::to_string(static_cast<int>(card_->progress * 100 + 0.5)) + "% · " + meta;
        return meta + " · " + openedAgo(cardOpenedMs_, wallMs_());
    }
    return cardHighlights_ ? meta + " · new" : "new";
}

void HomeScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    const int contentW = W() - 2 * kMargin;
    fillRect(cr, {0, 0, W(), H()}, 255);

    // Header: title, status line, settings, sync, close.
    drawText(cr, "Zotero", {kSans, 17, 700}, {kMargin, 18, contentW - 300, 100}, 1);
    const std::string line = status_.line(nowMs_);
    if (!line.empty()) drawText(cr, line, {kSans, 6.5, 400, false, kTertiary}, {kMargin + 4, 108, contentW - 300, 34}, 1);
    const Rect gear = settingsRect();
    gearIcon(cr, gear.x + gear.w / 2.0, gear.y + gear.h / 2.0, 22, kSecondary);
    status_.drawIndicator(cr, syncIndicatorRect());
    {
        const Rect x = closeRect();
        const double cx = x.x + x.w / 2.0, cy = x.y + x.h / 2.0, r = 17;
        gray(cr, kSecondary);
        cairo_set_line_width(cr, 5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, cx - r, cy - r);
        cairo_line_to(cr, cx + r, cy + r);
        cairo_move_to(cr, cx + r, cy - r);
        cairo_line_to(cr, cx - r, cy + r);
        cairo_stroke(cr);
    }

    // Search field.
    const Rect search = searchFieldRect();
    gray(cr, 170);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, search.x + 1.5, search.y + 1.5, search.w - 3, search.h - 3, search.h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, search.x + 54, search.y + search.h / 2.0 - 4, kTertiary);
    centredText(cr, "Search your library…", {kSans, 9, 400, false, kTertiary},
                {search.x + 96, search.y, search.w - 120, search.h}, Align::Left);

    if (total_ == 0) {
        std::string msg = "Your library is empty here. Tap ⟳ to sync.";
        if (status_.syncing()) {
            const std::string prefix = "Syncing…";
            msg = "Syncing your library…" + status_.line(nowMs_).substr(prefix.size());
        }
        drawText(cr, msg, {kSerif, 11, 400, false, kSecondary}, {kMargin + 40, 560, contentW - 80, 200}, 3, Align::Center);
    } else if (card_) {
        // Continue reading (or, before anything was opened, the newest readable item).
        const Rect cover{kMargin, kCardY, kCoverW, kCoverH};
        generatedCover(cr, cover, card_->title.empty() ? "Untitled" : card_->title, authorsOf(card_->byline), true);
        const int colX = cover.x + cover.w + 48, colW = W() - kMargin - colX;
        drawText(cr, cardOpened_ ? "CONTINUE READING" : "START READING", {kSans, 7, 700, false, kSecondary},
                 {colX, kCardY + 6, colW, 50}, 1);
        const Rect bar{colX, kCardY + kCoverH - 92, colW, 10};
        const TextStyle bylineStyle{kSans, 8, 400, false, kSecondary};
        const int bylineH = measureText(card_->byline.empty() ? " " : card_->byline, bylineStyle, colW, 1).height;
        const int bylineY = bar.y - 24 - bylineH;
        drawText(cr, card_->title.empty() ? "Untitled" : card_->title, {kSerif, 13, 700},
                 {colX, kCardY + 58, colW, bylineY - 16 - (kCardY + 58)}, 3);
        drawText(cr, card_->byline, bylineStyle, {colX, bylineY, colW, bylineH + 4}, 1);
        progressBar(cr, bar, card_->progress > 0 ? card_->progress : 0);
        drawText(cr, cardMeta(), {kSans, 7, 400, false, kSecondary}, {colX, kCardY + kCoverH - 64, colW, 70}, 2);
    }
    if (total_ > 0) {
        hRule(cr, kCardY + kCoverH + 44, 0, W());
        drawText(cr, "RECENT", {kSans, 7, 700, false, kSecondary}, {kMargin, kRecentLabelY, contentW, 50}, 1);
        auto rects = recentRowRects();
        for (size_t i = 0; i < rects.size(); ++i)
            drawItemRow(cr, rects[i], recent_[i], actions_.status ? actions_.status(recent_[i].attachmentKey) : "");
    }
    if (status_.hasError()) drawBanner(cr, bannerRect(), status_.error());
}

void HomeScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (closeRect().contains(g.pos)) {
        nav.quit();  // the launcher returns to the Kindle home
        return;
    }
    if (syncIndicatorRect().contains(g.pos)) {
        nav.sync().syncNow();
        return;
    }
    if (settingsRect().contains(g.pos)) {
        if (actions_.openSettings) actions_.openSettings();
        return;
    }
    if (searchFieldRect().contains(g.pos)) {
        if (actions_.openSearch) actions_.openSearch();
        return;
    }
    if (card_ && total_ > 0 && cardRect().contains(g.pos)) {
        if (actions_.openItem) actions_.openItem(*card_);
        return;
    }
    auto rects = recentRowRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos) && actions_.openItem) actions_.openItem(recent_[i]);
}

void HomeScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    nowMs_ = nav.nowMs();
    status_.onEvent(e, nowMs_);
    switch (e.kind) {
        case SyncEvent::Kind::SyncStarted:
        case SyncEvent::Kind::SyncProgress:
            nav.damage({0, 0, W(), kHeaderH}, Waveform::Ui);
            if (total_ == 0) {
                reload(nav.library());  // the first items may have arrived
                nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, total_ ? Waveform::Quality : Waveform::Ui);
            }
            break;
        case SyncEvent::Kind::SyncFinished:
            reload(nav.library());
            nav.redrawAll(Waveform::Quality);
            break;
        case SyncEvent::Kind::DownloadProgress: {
            auto rects = recentRowRects();
            for (size_t i = 0; i < rects.size(); ++i)
                if (recent_[i].attachmentKey == e.attachmentKey) nav.damage(rects[i], Waveform::Ui);
            break;
        }
        case SyncEvent::Kind::DownloadFinished:
        case SyncEvent::Kind::AnnotationsReady:
            reload(nav.library());
            nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, Waveform::Quality);
            break;
        case SyncEvent::Kind::PushStarted:  // "Uploading 3 changes…", then the header as before
            nav.damage({0, 0, W(), kHeaderH}, Waveform::Ui);
            break;
        case SyncEvent::Kind::PushFinished:
            nav.damage({0, 0, W(), kHeaderH}, Waveform::Quality);
            break;
        default: break;
    }
}

}  // namespace zk
