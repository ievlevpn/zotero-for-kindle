#pragma once
// Software update (opened from Settings when a newer release is found): its notes and size,
// Update / Not now; then the download's progress with Cancel; then "Installing…", and the app
// restarts into the new version. Keeps the Kindle awake while it downloads and installs.

#include "screens/screen.h"
#include "update/updater.h"

#include <functional>
#include <memory>

namespace zk {

class KeepAwake;
struct KeepAwakeScope;

class UpdateScreen final : public Screen {
public:
    // `stopSync` runs on the UI thread just before installing (the library is backed up then).
    // `resumeSync`: after an install that failed without changing anything.
    UpdateScreen(Updater&, std::function<void()> stopSync, KeepAwake*, std::function<void()> resumeSync = nullptr);
    ~UpdateScreen() override;
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onTick(Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }

    static Rect primaryRect();    // Update / Try again / Restart
    static Rect secondaryRect();  // Not now / Cancel

private:
    using Phase = Updater::Status::Phase;
    void leave(Nav&);
    Updater& updater_;
    std::function<void()> stopSync_, resumeSync_;
    KeepAwake* keepAwake_;
    std::unique_ptr<KeepAwakeScope> awake_;
    Updater::Status shown_;
    int shownPercent_ = -1;
    bool installRequested_ = false, restartRequested_ = false, syncResumed_ = false;
};

}  // namespace zk
