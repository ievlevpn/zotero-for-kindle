#pragma once
// The PIN pad (spec: multiple accounts, PIN): the app's number pad for an account's PIN of 4 to 6
// digits. It asks the PIN (the lock: at startup, on a switch, on wake), sets one (entered twice,
// with the honest limits shown), changes or removes one (the current PIN first). Wrong tries wait
// as AccountPins says (a countdown, the keys off meanwhile).
//
// Forgot PIN? explains the reset: the account's Zotero API key — which only its owner has — in a
// text file reset-pin.txt in the zotero folder over USB, or typed here. The file is read and
// deleted (match or not); a key that matches offline, or — typed, with Wi-Fi — one Zotero says is
// this user's (a key changed on zotero.org since: kept as the account's new key), offers Set a new
// PIN or Remove PIN. Any other key is "This key isn't this account's" and counts as a wrong try.

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "app/pin.h"
#include "net/errors.h"
#include "screens/account_sheet.h"
#include "screens/screen.h"
#include "sync/config.h"
#include "ui/text_field.h"

namespace zk {

struct PinPadDeps {
    int64_t userId = 0;
    std::string name;             // the account's, as the prompts say it
    AccountPins* pins = nullptr;  // required
    // The account's key on the Kindle (Forgot PIN?'s proof is compared with it).
    std::function<std::optional<Config>()> credentials;
    std::string resetFile;  // <USB>/zotero/reset-pin.txt
    // Whose a typed key is, asked of Zotero (checkKeyOwner): a key that isn't the one on the Kindle
    // counts as a wrong try at once; Zotero saying it is this user's (a new key) only takes it back.
    // Unset, or offline (`online` false): typed keys are compared offline only.
    std::function<UserError(const Config&)> checkKey;
    std::function<bool()> online;
    // A new key Zotero confirmed for this user: kept as the account's key.
    std::function<void(const Config&)> keepKey;
    std::function<void(Nav&)> done;  // unlocked; the PIN set, changed or removed; the PIN was right
    // ‹ in the header (unset: none — the lock has no way past it but the PIN or another account).
    std::function<void(Nav&)> back;
    // Use another account: the other accounts (none: the link is hidden) and a choice among them.
    std::function<std::vector<AccountSheet::Row>()> others;
    std::function<void(int64_t userId, Nav&)> useAnother;
    // Unlock of an account whose PIN isn't known (needsKey: the account list was made again): no
    // PIN to enter — the pad starts at the key (reset-pin.txt or typed), and the account's key
    // proved is `done`.
    bool keyOnly = false;
};

class PinPadScreen final : public Screen {
public:
    enum class Purpose {
        Unlock,  // the lock: the PIN, Forgot PIN?, Use another account
        Verify,  // the PIN before something that needs it (removing the account)
        Set,     // a new PIN, twice
        Change,  // the current PIN, then a new one twice
        Remove,  // the current PIN, then the PIN is removed
    };
    enum class Stage { Enter, Choose, Confirm, Forgot, TypeKey, Reset };

    static constexpr const char* kLimits =
        "The PIN locks the app's screens. Files on this Kindle can still be read over USB; your API key can't.";

    PinPadScreen(Purpose, PinPadDeps);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    // "0"–"9", "⌫", "OK".
    Rect keyRect(const std::string& key) const;
    Rect forgotRect() const;   // "Forgot PIN?" (Unlock only)
    Rect anotherRect() const;  // "Use another account" (Unlock, with others; keyOnly: under the key's choices)
    // Forgot PIN?: check the file, type the key; the typed key's field and Check; then Set a new PIN / Remove PIN.
    Rect closeRect() const;  // shown only without ‹ (a lock with no way back): closes Zotero
    bool canGoBack() const;
    Rect checkFileRect() const;
    Rect typeKeyRect() const;
    Rect keyFieldRect() const;
    Rect checkKeyRect() const;
    Rect newPinRect() const;
    Rect removePinRect() const;

    Stage stage() const { return stage_; }
    const std::string& entry() const { return entry_; }
    std::string title() const;
    std::string prompt() const;
    const std::string& message() const { return message_; }  // "Wrong PIN. Try again in 30 s", …
    bool waiting() const { return waitLeftMs_ > 0; }
    const AccountSheet* chooser() const { return chooser_ ? &*chooser_ : nullptr; }

private:
    void press(const std::string& key, Nav&);
    void submit(Nav&);
    void checkFile(Nav&);
    void checkTypedKey(Nav&);
    void keyAccepted(Nav&);
    void refreshWait(Nav&);
    void to(Stage, Nav&);
    Rect linkRow() const;

    Purpose purpose_;
    PinPadDeps deps_;
    Stage stage_ = Stage::Enter;
    std::string entry_, first_;  // what is typed; Confirm: the PIN chosen first
    std::string message_;
    bool messageIsError_ = false;
    int64_t waitLeftMs_ = 0, waitShownS_ = -1;
    TextField key_;
    bool typing_ = false;
    std::optional<AccountSheet> chooser_;
    // The online check of a typed key runs on a detached thread; leaving the screen never waits.
    struct Check {
        std::mutex mutex;
        bool done = false;
        UserError result;
    };
    std::shared_ptr<Check> pending_;
    std::optional<Config> checking_;  // the key being checked online
};

}  // namespace zk
