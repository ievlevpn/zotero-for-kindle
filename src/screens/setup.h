#pragma once
// First-run Setup (spec §7.7): Zotero user ID + API key, WebDAV URL + user + password. A
// config.json dropped over USB prefills it, and the screen only asks to Connect. Connect checks
// the key (and WebDAV, if given) on a background thread and shows any problem in plain words.

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

// The checks behind Connect; ErrorKind::None when everything works.
UserError checkAccount(Http&, const Config&);

class SetupScreen final : public Screen {
public:
    using Checker = std::function<UserError(const Config&)>;
    using Connected = std::function<void(const Config&, Nav&)>;
    static constexpr int kFields = 5;

    SetupScreen(Config initial, bool imported, std::string configDir, Checker, Connected, bool canGoBack = false);

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
};

}  // namespace zk
