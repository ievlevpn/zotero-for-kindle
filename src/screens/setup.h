#pragma once
// First-run Setup (spec §7.7): Zotero user ID + API key, WebDAV URL + user + password. A
// config.json dropped over USB prefills it, and the screen only asks to Connect. Connect checks
// the key (and WebDAV, if given) on a background thread and shows any problem in plain words.
// The same screen adds an account (a user already on the Kindle is refused: "Pavel is already on
// this Kindle") and takes a new key for one (Update key: the same user only, "This key is for
// another account — use Add account"); neither writes config.json (spec: multiple accounts).

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "net/http.h"
#include "screens/screen.h"
#include "sync/config.h"
#include "ui/text_field.h"

namespace zk {

// The checks behind Connect; ErrorKind::None when everything works. `name`: the key's Zotero
// username (its display name when it has none), once the key is read.
UserError checkAccount(Http&, const Config&, std::string* name = nullptr);
// Only whose the key is: ErrorKind::None when Zotero says it is `c.userId`'s (details "userID
// mismatch" when another user's; the key's other rights aren't asked).
UserError checkKeyOwner(Http&, const Config& c);

struct SetupPurpose {
    enum class Kind { First, Add, Update };
    Kind kind = Kind::First;
    int64_t userId = 0;  // Update: the account's
    std::string name;    // Update: the account's
    // Add: the name of the account on this Kindle with this user ID ("" when there is none).
    std::function<std::string(int64_t userId)> present;
    // First: the key is also written to config.json (a first setup, which the account layout takes
    // in); not after the last account was removed (the account list is there).
    bool keepsConfigFile = true;
};

class SetupScreen final : public Screen {
public:
    using Checker = std::function<UserError(const Config&)>;
    using Connected = std::function<void(const Config&, Nav&)>;
    static constexpr int kFields = 5;

    SetupScreen(Config initial, bool imported, std::string configDir, Checker, Connected, bool canGoBack = false);
    void setPurpose(SetupPurpose p) { purpose_ = std::move(p); }
    const SetupPurpose& purpose() const { return purpose_; }

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    Config config() const;
    std::string shownValue(int field) const;  // the password as dots
    const std::optional<UserError>& error() const { return error_; }
    bool checking() const { return checking_; }

    static Rect fieldRect(int i);
    static Rect connectRect();
    static Rect closeRect();  // shown only without a back button: closes Zotero

private:
    void connect(Nav&);

    TextField fields_[kFields];
    bool imported_;
    std::string configDir_;
    Checker checker_;
    Connected connected_;
    int focus_ = -1;
    bool checking_ = false;
    bool canGoBack_ = false;
    // The check runs on a detached thread; leaving the screen never waits for the network.
    struct Check {
        std::mutex mutex;
        bool done = false;
        UserError result;
    };
    std::shared_ptr<Check> pending_;
    std::optional<UserError> error_;
    SetupPurpose purpose_;
};

}  // namespace zk
