#include "screens/pin_pad.h"

#include <algorithm>
#include <thread>

#include "core/log.h"
#include "core/wake.h"
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
const char* const kKeys[4][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"⌫", "0", "OK"}};
constexpr int kGap = 24, kLinkH = 90;
constexpr int kPromptY = kHeaderH + 10, kDotsY = kHeaderH + 130, kMessageY = kHeaderH + 220, kPadY = kHeaderH + 340;
const char* const kNotThisAccount = "This key isn't this account's";

int keyW() { return std::min(300, (W() - 2 * kMargin - 2 * kGap) / 3); }
int keyH() { return std::clamp((H() - kPadY - 30 - kLinkH - 30 - 3 * kGap) / 4, 100, 150); }

// "30 s", "4 min".
std::string waitText(int64_t ms) {
    const int64_t s = (ms + 999) / 1000;
    if (s <= 90) return std::to_string(s) + " s";
    return std::to_string((s + 59) / 60) + " min";
}

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

}  // namespace

PinPadScreen::PinPadScreen(Purpose purpose, PinPadDeps deps) : purpose_(purpose), deps_(std::move(deps)) {
    stage_ = purpose_ == Purpose::Set ? Stage::Choose : deps_.keyOnly ? Stage::Forgot : Stage::Enter;
}

Rect PinPadScreen::keyRect(const std::string& key) const {
    const int w = keyW(), h = keyH(), left = (W() - 3 * w - 2 * kGap) / 2;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            if (key == kKeys[r][c]) return {left + c * (w + kGap), kPadY + r * (h + kGap), w, h};
    return {};
}

Rect PinPadScreen::linkRow() const {
    const Rect last = keyRect("0");
    return {kMargin, last.y + last.h + 30, W() - 2 * kMargin, kLinkH};
}

Rect PinPadScreen::forgotRect() const {
    if (purpose_ != Purpose::Unlock || stage_ != Stage::Enter) return {};
    const Rect row = linkRow();
    return {row.x, row.y, row.w / 2, row.h};
}

Rect PinPadScreen::anotherRect() const {
    if (purpose_ != Purpose::Unlock || !deps_.others || !deps_.useAnother) return {};
    if (deps_.keyOnly && stage_ == Stage::Forgot) {
        const Rect k = typeKeyRect();
        return {kMargin, k.y + k.h + 30, W() - 2 * kMargin, kLinkH};
    }
    if (stage_ != Stage::Enter) return {};
    const Rect row = linkRow();
    return {row.x + row.w / 2, row.y, row.w - row.w / 2, row.h};
}

bool PinPadScreen::canGoBack() const {
    return deps_.back || (stage_ == Stage::Forgot && !deps_.keyOnly) || stage_ == Stage::TypeKey || stage_ == Stage::Reset;
}
// Top right, where Home has it: a lock with no ‹ (on wake, or the only account at startup) still
// lets Zotero be closed. Closing passes no lock: the next start asks the PIN again.
Rect PinPadScreen::closeRect() const { return {W() - kMargin - 80, 34, 80, 96}; }
Rect PinPadScreen::checkFileRect() const { return {kMargin, kHeaderH + 700, W() - 2 * kMargin, 100}; }
Rect PinPadScreen::typeKeyRect() const { return {kMargin, kHeaderH + 830, W() - 2 * kMargin, 100}; }
Rect PinPadScreen::keyFieldRect() const { return {kMargin, kHeaderH + 110, W() - 2 * kMargin, 100}; }
Rect PinPadScreen::checkKeyRect() const { return {kMargin, kHeaderH + 250, W() - 2 * kMargin, 100}; }
Rect PinPadScreen::newPinRect() const { return {kMargin, kHeaderH + 200, W() - 2 * kMargin, 100}; }
Rect PinPadScreen::removePinRect() const { return {kMargin, kHeaderH + 330, W() - 2 * kMargin, 100}; }

std::string PinPadScreen::title() const {
    switch (stage_) {
        case Stage::Forgot:
        case Stage::TypeKey:
        case Stage::Reset: return deps_.keyOnly ? deps_.name : "Forgot PIN?";
        case Stage::Choose:
        case Stage::Confirm: return purpose_ == Purpose::Change ? "Change PIN" : "Set a PIN";
        case Stage::Enter: break;
    }
    switch (purpose_) {
        case Purpose::Change: return "Change PIN";
        case Purpose::Remove: return "Remove PIN";
        default: return deps_.name;
    }
}

std::string PinPadScreen::prompt() const {
    switch (stage_) {
        case Stage::Enter: return "Enter the PIN for " + deps_.name;
        case Stage::Choose: return "Choose a PIN of 4 to 6 digits for " + deps_.name;
        case Stage::Confirm: return "Enter the same PIN again";
        case Stage::Forgot:
            if (deps_.keyOnly)
                return "The list of accounts on this Kindle had to be made again, so " + deps_.name +
                       " opens only with its Zotero API key — only its owner has it:\n"
                       "1. Connect the Kindle to a computer by USB.\n"
                       "2. In the zotero folder, make a text file named reset-pin.txt with the API key in it "
                       "(zotero.org/settings/keys).\n"
                       "3. Eject the Kindle, then tap Check reset-pin.txt.\n"
                       "Or type the key here.";
            return "To set a new PIN, show the app " + deps_.name +
                   "'s Zotero API key — only its owner has it:\n"
                   "1. Connect the Kindle to a computer by USB.\n"
                   "2. In the zotero folder, make a text file named reset-pin.txt with the API key in it "
                   "(zotero.org/settings/keys).\n"
                   "3. Eject the Kindle, then tap Check reset-pin.txt.\n"
                   "Or type the key here. A key made new on zotero.org since works too, with Wi-Fi.";
        case Stage::TypeKey: return "The Zotero API key of " + deps_.name;
        case Stage::Reset: return "The key is " + deps_.name + "'s. Set a new PIN, or remove the PIN.";
    }
    return {};
}

void PinPadScreen::onShow(Nav& nav) { refreshWait(nav); }

// The wait as AccountPins has it, and its line; the keys are off while it runs.
void PinPadScreen::refreshWait(Nav& nav) {
    if (!deps_.pins) return;
    const int64_t left = deps_.pins->waitMs(deps_.userId);
    const bool was = waitLeftMs_ > 0;
    waitLeftMs_ = left;
    if (left > 0) {
        const int64_t s = (left + 999) / 1000;
        if (s != waitShownS_) {
            waitShownS_ = s;
            message_ = "Wrong PIN. Try again in " + waitText(left);
            messageIsError_ = true;
            nav.redrawAll(Waveform::Ui);
        }
        nav.wakeAt(nav.nowMs() + std::min<int64_t>(1000, left));
    } else if (was) {
        waitShownS_ = -1;
        message_.clear();
        nav.redrawAll(Waveform::Ui);
    }
}

void PinPadScreen::onTick(Nav& nav) {
    if (waitLeftMs_ > 0) refreshWait(nav);
    if (!pending_) return;
    UserError verdict;
    {
        std::lock_guard<std::mutex> lock(pending_->mutex);
        if (!pending_->done) return;
        verdict = pending_->result;
    }
    pending_.reset();
    const Config c = *checking_;
    checking_.reset();
    if (verdict.kind == ErrorKind::None) {
        // Zotero says the key is this user's: the account's new key, kept, and the proof (the wrong
        // try it was counted as is forgiven).
        if (deps_.keepKey) deps_.keepKey(c);
        deps_.pins->keyProved(deps_.userId);
        return keyAccepted(nav);
    }
    // Not this user's, or no answer: the wrong try counted already stands.
    message_ = kNotThisAccount;
    if (const int64_t wait = deps_.pins->waitMs(deps_.userId); wait > 0) message_ += ". Try again in " + waitText(wait);
    messageIsError_ = true;
    waitShownS_ = -1;
    refreshWait(nav);
    nav.redrawAll(Waveform::Ui);
}

void PinPadScreen::to(Stage s, Nav& nav) {
    stage_ = s;
    entry_.clear();
    message_.clear();
    waitShownS_ = -1;
    if (s == Stage::TypeKey) {
        key_.clear();
        typing_ = true;
        nav.keyboard().show();
    } else if (typing_) {
        typing_ = false;
        nav.keyboard().hide();
    }
    refreshWait(nav);
    if (waitLeftMs_ > 0 && (s == Stage::Choose || s == Stage::Confirm || s == Stage::Reset)) message_.clear();
    nav.redrawAll(Waveform::Quality);
}

void PinPadScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, title(), canGoBack());
    if (!canGoBack()) {
        const Rect x = closeRect();
        closeIcon(cr, x.x + x.w / 2.0, x.y + x.h / 2.0, kSecondary);
    }
    const int w = W() - 2 * kMargin;
    const TextStyle body{kSans, 8, 400}, quiet{kSans, 7, 400, false, kSecondary}, error{kSans, 7.5, 700};

    if (stage_ == Stage::Forgot) {
        drawText(cr, prompt(), body, {kMargin, kHeaderH + 20, w, 660}, 16);
        drawButton(cr, checkFileRect(), "Check reset-pin.txt", true);
        drawButton(cr, typeKeyRect(), "Type the key", false);
        int msgY = typeKeyRect().y + 130;
        if (const Rect a = anotherRect(); !a.empty()) {
            centredText(cr, "Use another account", {kSans, 8, 500, false, kSecondary}, a, Align::Left);
            msgY = a.y + a.h + 10;
        }
        if (!message_.empty()) drawText(cr, message_, error, {kMargin, msgY, w, 100}, 2);
    } else if (stage_ == Stage::TypeKey) {
        drawText(cr, prompt(), {kSans, 7, 500, false, kSecondary}, {kMargin, kHeaderH + 40, w, 60}, 1);
        const Rect f = keyFieldRect();
        gray(cr, kInk);
        cairo_set_line_width(cr, 4);
        roundedRect(cr, f.x + 2, f.y + 2, f.w - 4, f.h - 4, 14);
        cairo_stroke(cr);
        centredText(cr, key_.text() + (typing_ ? "|" : ""), {kSans, 8.5, 400}, {f.x + 28, f.y, f.w - 56, f.h}, Align::Left);
        drawButton(cr, checkKeyRect(), pending_ ? "Checking…" : "Check", true);
        if (!message_.empty()) drawText(cr, message_, error, {kMargin, checkKeyRect().y + 130, w, 100}, 2);
    } else if (stage_ == Stage::Reset) {
        drawText(cr, prompt(), body, {kMargin, kHeaderH + 40, w, 120}, 3);
        drawButton(cr, newPinRect(), "Set a new PIN", true);
        drawButton(cr, removePinRect(), "Remove PIN", false);
        if (!message_.empty()) drawText(cr, message_, error, {kMargin, removePinRect().y + 130, w, 100}, 2);
    } else {
        drawText(cr, prompt(), {kSans, 8.5, 500}, {kMargin, kPromptY, w, 110}, 2, Align::Center);
        // The digits typed, as dots (four places at least).
        const int places = std::max<int>(4, static_cast<int>(entry_.size()));
        const int step = 70;
        const double x0 = W() / 2.0 - (places - 1) * step / 2.0, cy = kDotsY + 40;
        for (int i = 0; i < places; ++i) {
            gray(cr, kInk);
            cairo_new_path(cr);
            cairo_arc(cr, x0 + i * step, cy, 18, 0, 2 * 3.14159265);
            if (i < static_cast<int>(entry_.size())) {
                cairo_fill(cr);
            } else {
                cairo_set_line_width(cr, 3);
                cairo_stroke(cr);
            }
        }
        const Rect msg{kMargin, kMessageY, w, 110};
        if (!message_.empty()) drawText(cr, message_, error, msg, 2, Align::Center);
        else if (stage_ == Stage::Choose) drawText(cr, kLimits, quiet, msg, 3, Align::Center);  // shown when setting a PIN
        const bool off = waitLeftMs_ > 0;
        for (const auto& row : kKeys)
            for (const char* k : row) {
                const Rect r = keyRect(k);
                if (off) {  // the keys wait with the user
                    fillRect(cr, r, 255);
                    gray(cr, 200);
                    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 14);
                    cairo_set_line_width(cr, 3);
                    cairo_stroke(cr);
                    centredText(cr, k, {kSans, 11, 700, false, 170}, r);
                } else {
                    fillRect(cr, r, 255);
                    gray(cr, std::string(k) == "OK" ? kInk : 120);
                    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 14);
                    if (std::string(k) == "OK") {
                        cairo_fill(cr);
                    } else {
                        cairo_set_line_width(cr, 3);
                        cairo_stroke(cr);
                    }
                    centredText(cr, k, {kSans, 11, 700, false, static_cast<uint8_t>(std::string(k) == "OK" ? 255 : kInk)}, r);
                }
            }
        const TextStyle link{kSans, 8, 500, false, kSecondary};
        if (const Rect f = forgotRect(); !f.empty()) centredText(cr, "Forgot PIN?", link, f, Align::Left);
        if (const Rect a = anotherRect(); !a.empty()) centredText(cr, "Use another account", link, a, Align::Right);
    }
    if (chooser_) chooser_->draw(cr, {0, 0, W(), H()});
}

void PinPadScreen::onGesture(const Gesture& g, Nav& nav) {
    if (chooser_) {
        const Rect area{0, 0, W(), H()};
        switch (chooser_->onGesture(g, area)) {
            case AccountSheet::Action::Close:
                chooser_.reset();
                nav.redrawAll(Waveform::Ui);
                break;
            case AccountSheet::Action::Switch: {
                const int64_t id = chooser_->chosen();
                chooser_.reset();
                if (deps_.useAnother) deps_.useAnother(id, nav);
                break;
            }
            default: break;
        }
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        switch (stage_) {
            case Stage::Forgot:
                if (deps_.keyOnly) {
                    if (deps_.back) deps_.back(nav);
                    return;
                }
                return to(Stage::Enter, nav);
            case Stage::Reset: return to(Stage::Enter, nav);
            case Stage::TypeKey: return to(Stage::Forgot, nav);
            default:
                if (deps_.back) deps_.back(nav);
                return;
        }
    }
    if (!canGoBack() && closeRect().contains(g.pos)) {
        nav.keyboard().hide();
        nav.quit();  // the launcher returns to the Kindle home
        return;
    }
    auto chooseAnother = [&] {
        auto others = deps_.others();
        if (others.empty()) return;
        if (others.size() == 1 && !others.front().locked) return deps_.useAnother(others.front().userId, nav);
        chooser_.emplace(std::move(others), false, false, "Use another account");
        nav.redrawAll(Waveform::Ui);
    };
    if (stage_ == Stage::Forgot) {
        if (checkFileRect().contains(g.pos)) return checkFile(nav);
        if (typeKeyRect().contains(g.pos)) return to(Stage::TypeKey, nav);
        if (const Rect a = anotherRect(); !a.empty() && a.contains(g.pos)) return chooseAnother();
        return;
    }
    if (stage_ == Stage::TypeKey) {
        if (checkKeyRect().contains(g.pos)) return checkTypedKey(nav);
        if (keyFieldRect().contains(g.pos) && !typing_) {
            typing_ = true;
            nav.keyboard().show();
            nav.damage(keyFieldRect(), Waveform::Ui);
        }
        return;
    }
    if (stage_ == Stage::Reset) {
        if (newPinRect().contains(g.pos)) {
            purpose_ = Purpose::Set;  // then done: unlocked with the new PIN
            return to(Stage::Choose, nav);
        }
        if (removePinRect().contains(g.pos)) {
            if (!deps_.pins->removePin(deps_.userId)) {
                message_ = "Can't change the accounts on the Kindle.";
                nav.redrawAll(Waveform::Ui);
                return;
            }
            if (deps_.done) deps_.done(nav);
        }
        return;
    }
    if (const Rect f = forgotRect(); !f.empty() && f.contains(g.pos)) return to(Stage::Forgot, nav);
    if (const Rect a = anotherRect(); !a.empty() && a.contains(g.pos)) return chooseAnother();
    for (const auto& row : kKeys)
        for (const char* k : row)
            if (keyRect(k).contains(g.pos)) return press(k, nav);
}

void PinPadScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (stage_ == Stage::TypeKey) {
        if (k.code == KeyEvent::Code::Return) return checkTypedKey(nav);
        if (k.code == KeyEvent::Code::Escape) return to(Stage::Forgot, nav);
        key_.apply(k);
        nav.damage(keyFieldRect(), Waveform::Fast);
        return;
    }
    if (stage_ != Stage::Enter && stage_ != Stage::Choose && stage_ != Stage::Confirm) return;
    if (k.code == KeyEvent::Code::Backspace) return press("⌫", nav);
    if (k.code == KeyEvent::Code::Return) return press("OK", nav);
    if (k.code == KeyEvent::Code::Text && k.text.size() == 1 && k.text[0] >= '0' && k.text[0] <= '9') press(k.text, nav);
}

void PinPadScreen::press(const std::string& key, Nav& nav) {
    if (waitLeftMs_ > 0) return;
    if (key == "OK") return submit(nav);
    if (!messageIsError_ || key != "⌫") message_.clear();
    if (key == "⌫") {
        if (!entry_.empty()) entry_.pop_back();
    } else if (entry_.size() < 6) {
        entry_ += key;
        if (!message_.empty() && messageIsError_) message_.clear();
    }
    nav.damage({0, kDotsY, W(), kPadY - kDotsY}, Waveform::Fast);
}

void PinPadScreen::submit(Nav& nav) {
    messageIsError_ = true;
    if (!validPinFormat(entry_)) {
        message_ = "A PIN is 4 to 6 digits.";
        nav.damage({0, kDotsY, W(), kPadY - kDotsY}, Waveform::Ui);
        return;
    }
    const std::string pin = entry_;
    entry_.clear();
    if (stage_ == Stage::Choose) {
        first_ = pin;
        return to(Stage::Confirm, nav);
    }
    if (stage_ == Stage::Confirm) {
        if (pin != first_) {
            first_.clear();
            to(Stage::Choose, nav);
            message_ = "The PINs didn't match. Choose a PIN again.";
            return;
        }
        first_.clear();
        if (!deps_.pins->setPin(deps_.userId, pin)) {
            message_ = "Can't keep the PIN on the Kindle.";
            nav.redrawAll(Waveform::Ui);
            return;
        }
        if (deps_.done) deps_.done(nav);
        return;
    }
    switch (deps_.pins->tryPin(deps_.userId, pin)) {
        case AccountPins::Result::Ok:
            message_.clear();
            if (purpose_ == Purpose::Change) return to(Stage::Choose, nav);
            if (purpose_ == Purpose::Remove && !deps_.pins->removePin(deps_.userId)) {
                message_ = "Can't change the accounts on the Kindle.";
                nav.redrawAll(Waveform::Ui);
                return;
            }
            if (deps_.done) deps_.done(nav);
            return;
        case AccountPins::Result::Wrong:
            message_ = "Wrong PIN.";
            waitShownS_ = -1;
            refreshWait(nav);  // a wait from this miss on: its countdown in place of the message
            break;
        case AccountPins::Result::Waiting:
            waitShownS_ = -1;
            refreshWait(nav);
            break;
        case AccountPins::Result::Failed: message_ = "Can't read the accounts on the Kindle."; break;
    }
    nav.redrawAll(Waveform::Ui);
}

void PinPadScreen::checkFile(Nav& nav) {
    messageIsError_ = true;
    const std::optional<std::string> key = takeResetKey(deps_.resetFile);  // gone now, match or not, wait or not
    if (!key) {
        message_ = "There's no reset-pin.txt in the zotero folder.";
        nav.redrawAll(Waveform::Ui);
        return;
    }
    if (const int64_t wait = deps_.pins->waitMs(deps_.userId); wait > 0) {  // not even looked at
        message_ = "Wrong tries: put reset-pin.txt back and try again in " + waitText(wait);
        nav.redrawAll(Waveform::Ui);
        return;
    }
    const Config stored = deps_.credentials ? deps_.credentials().value_or(Config{}) : Config{};
    if (deps_.pins->tryKey(deps_.userId, stored, *key) == AccountPins::Result::Ok) {
        return keyAccepted(nav);
    }
    log(LogLevel::Info, "accounts: a reset key for %lld didn't match", static_cast<long long>(deps_.userId));
    message_ = kNotThisAccount;
    if (const int64_t wait = deps_.pins->waitMs(deps_.userId); wait > 0) message_ += ". Try again in " + waitText(wait);
    nav.redrawAll(Waveform::Ui);
}

void PinPadScreen::checkTypedKey(Nav& nav) {
    if (pending_) return;
    messageIsError_ = true;
    const std::string typed = trimmed(key_.text());
    if (typed.empty()) return;
    typing_ = false;
    nav.keyboard().hide();
    if (const int64_t wait = deps_.pins->waitMs(deps_.userId); wait > 0) {
        message_ = "Wrong tries: try again in " + waitText(wait);
        nav.redrawAll(Waveform::Ui);
        return;
    }
    const Config stored = deps_.credentials ? deps_.credentials().value_or(Config{}) : Config{};
    // Not the key on the Kindle: a wrong try, counted now whatever Zotero says (or doesn't) later.
    if (deps_.pins->tryKey(deps_.userId, stored, typed) == AccountPins::Result::Ok) {
        return keyAccepted(nav);
    }
    message_ = kNotThisAccount;
    if (const int64_t wait = deps_.pins->waitMs(deps_.userId); wait > 0) message_ += ". Try again in " + waitText(wait);
    if (!deps_.checkKey || (deps_.online && !deps_.online())) {
        nav.redrawAll(Waveform::Ui);
        return;
    }
    // Perhaps a key made new on zotero.org since: Zotero says whose it is (only that rescues it).
    message_ = "Not the key on this Kindle: asking Zotero whether it's a new key of " + deps_.name + "…";
    messageIsError_ = false;
    Config c = stored;
    c.userId = deps_.userId;
    c.apiKey = typed;
    checking_ = c;
    auto check = std::make_shared<Check>();
    pending_ = check;
    std::thread([check, checker = deps_.checkKey, c] {
        UserError r = checker(c);
        {
            std::lock_guard<std::mutex> lock(check->mutex);
            check->result = std::move(r);
            check->done = true;
        }
        wakeUi();
    }).detach();
    nav.redrawAll(Waveform::Ui);
}

void PinPadScreen::keyAccepted(Nav& nav) {
    log(LogLevel::Info, "accounts: the reset key of %lld matched", static_cast<long long>(deps_.userId));
    if (deps_.keyOnly) {  // its owner is proved: it opens (a PIN can be set in Settings → Accounts)
        if (deps_.done) deps_.done(nav);
        return;
    }
    to(Stage::Reset, nav);
}

}  // namespace zk
