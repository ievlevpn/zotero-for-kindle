#include "screens/setup.h"

#include <thread>

#include "core/wake.h"
#include "sync/webdav.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "zotero/api.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }
constexpr int kFieldsY = 300, kFieldStep = 145;
const char* kLabels[SetupScreen::kFields] = {"Zotero user ID", "API key", "WebDAV URL (empty: Zotero storage)", "WebDAV user",
                                             "WebDAV password"};

}  // namespace

UserError checkAccount(Http& http, const Config& c) {
    if (c.userId <= 0 || c.apiKey.empty())
        return {ErrorKind::Other, "Enter your Zotero user ID and API key (zotero.org/settings/keys).", "missing fields"};
    ZoteroApi api(http, c.userId, c.apiKey);
    api.setSleeper([](int) {});
    UserError err;
    auto key = api.keyInfo(&err);
    if (!key) return err;
    if (!key->canRead) return {ErrorKind::ApiKeyRejected, "This API key can't read your library.", "access.user.library false"};
    if (key->userId != c.userId)
        return {ErrorKind::ApiKeyRejected,
                "This API key belongs to another Zotero user (ID " + std::to_string(key->userId) + ").", "userID mismatch"};
    if (!c.webdavUrl.empty()) {
        WebDav dav(http, c.webdavUrl, c.webdavUser, c.webdavPassword);
        if (!dav.check(&err)) return err;
    } else if (!key->canReadFiles) {  // books come from Zotero storage
        return {ErrorKind::ApiKeyRejected,
                "This API key can't download files. Allow file access for it at zotero.org/settings/keys, or set up WebDAV.",
                "access.user.files false"};
    }
    return {};
}

SetupScreen::SetupScreen(Config initial, bool imported, std::string configDir, Checker checker, Connected connected,
                         bool canGoBack)
    : imported_(imported), configDir_(std::move(configDir)), checker_(std::move(checker)), connected_(std::move(connected)),
      canGoBack_(canGoBack) {
    if (initial.userId > 0) fields_[0].setText(std::to_string(initial.userId));
    fields_[1].setText(initial.apiKey);
    fields_[2].setText(initial.webdavUrl);
    fields_[3].setText(initial.webdavUser);
    fields_[4].setText(initial.webdavPassword);
}

Rect SetupScreen::fieldRect(int i) { return {kMargin, kFieldsY + i * kFieldStep + 44, W() - 2 * kMargin, 88}; }
Rect SetupScreen::connectRect() { return {kMargin, kFieldsY + kFields * kFieldStep + 20, W() - 2 * kMargin, 100}; }

Config SetupScreen::config() const {
    Config c;
    c.userId = std::strtoll(fields_[0].text().c_str(), nullptr, 10);
    c.apiKey = fields_[1].text();
    c.webdavUrl = fields_[2].text();
    c.webdavUser = fields_[3].text();
    c.webdavPassword = fields_[4].text();
    return c;
}

std::string SetupScreen::shownValue(int i) const {
    if (i == 1 && focus_ != 1 && fields_[1].text().size() > 4) {  // the API key: only its end, unless editing
        const std::string& k = fields_[1].text();
        return std::string("••••••••••••") + k.substr(k.size() - 4);
    }
    if (i != 4) return fields_[i].text();
    std::string dots;
    for (size_t n = 0; n < fields_[4].text().size(); ++n)
        if ((static_cast<unsigned char>(fields_[4].text()[n]) & 0xC0) != 0x80) dots += "•";
    return dots;
}

void SetupScreen::connect(Nav& nav) {
    if (checking_) return;
    nav.keyboard().hide();
    focus_ = -1;
    checking_ = true;
    error_.reset();
    const Config c = config();
    auto check = std::make_shared<Check>();
    pending_ = check;
    std::thread([check, checker = checker_, c] {
        UserError r = checker(c);
        {
            std::lock_guard<std::mutex> lock(check->mutex);
            check->result = std::move(r);
            check->done = true;
        }
        wakeUi();  // onTick takes the verdict
    }).detach();
    nav.redrawAll(Waveform::Quality);
}

void SetupScreen::onTick(Nav& nav) {
    if (!checking_ || !pending_) return;
    UserError verdict;
    {
        std::lock_guard<std::mutex> lock(pending_->mutex);
        if (!pending_->done) return;
        verdict = pending_->result;
    }
    checking_ = false;
    pending_.reset();
    if (verdict.kind != ErrorKind::None) {
        error_ = verdict;
        nav.redrawAll(Waveform::Quality);
        return;
    }
    const Config c = config();
    if (!saveConfig(configDir_, c)) {
        error_ = UserError{ErrorKind::DiskFull, "Can't save the settings on the Kindle.", configDir_};
        nav.redrawAll(Waveform::Quality);
        return;
    }
    if (connected_) connected_(c, nav);
}

void SetupScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Set up Zotero", canGoBack_);
    const std::string intro = imported_ ? "Settings loaded from config.json. Connect to check them."
                                        : "Create an API key at zotero.org/settings/keys with access to your library.";
    drawText(cr, intro, {kSans, 8, 400, false, kSecondary}, {kMargin, kHeaderH + 10, W() - 2 * kMargin, 120}, 3);
    for (int i = 0; i < kFields; ++i) {
        const Rect f = fieldRect(i);
        drawText(cr, kLabels[i], {kSans, 7, 500, false, kSecondary}, {f.x, f.y - 42, f.w, 40}, 1);
        gray(cr, focus_ == i ? kInk : 170);
        cairo_set_line_width(cr, focus_ == i ? 4 : 3);
        roundedRect(cr, f.x + 2, f.y + 2, f.w - 4, f.h - 4, 14);
        cairo_stroke(cr);
        const std::string v = shownValue(i) + (focus_ == i ? "|" : "");
        centredText(cr, v, {kSans, 8.5, 400}, {f.x + 28, f.y, f.w - 56, f.h}, Align::Left);
    }
    drawButton(cr, connectRect(), checking_ ? "Checking…" : "Connect", true);
    if (error_) drawBanner(cr, {0, connectRect().y + connectRect().h + 30, W(), 150}, *error_);
}

void SetupScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (canGoBack_ && headerBackRect().contains(g.pos)) {
        nav.keyboard().hide();
        nav.pop();
        return;
    }
    if (checking_) return;
    if (connectRect().contains(g.pos)) {
        connect(nav);
        return;
    }
    for (int i = 0; i < kFields; ++i)
        if (fieldRect(i).contains(g.pos)) {
            focus_ = i;
            nav.keyboard().show();
            nav.redrawAll(Waveform::Quality);
            return;
        }
    if (focus_ >= 0) {
        focus_ = -1;
        nav.keyboard().hide();
        nav.redrawAll(Waveform::Quality);
    }
}

void SetupScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (focus_ < 0) return;
    if (k.code == KeyEvent::Code::Return || k.code == KeyEvent::Code::Escape) {
        focus_ = k.code == KeyEvent::Code::Return && focus_ + 1 < kFields ? focus_ + 1 : -1;
        if (focus_ < 0) nav.keyboard().hide();
        nav.redrawAll(Waveform::Quality);
        return;
    }
    fields_[focus_].apply(k);
    nav.damage(fieldRect(focus_), Waveform::Fast);
}

}  // namespace zk
