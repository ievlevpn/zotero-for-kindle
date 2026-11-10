#pragma once
// A plugin's screen (zk.screen): the header with the screen's title and the plugin's name (so it
// can't pass for the app's own confirmation), and the plugin's blocks below (plugin_blocks.h).
// The plugin thread runs render/onEvent; this screen only sends what the user did and draws the
// trees that come back (through a ScreenLink: PluginEvents over the PluginHost).
//
// - "Loading…" until the first tree; "<Plugin> can't show this screen right now" with Back when
//   the host says so (ScreenError). An onEvent error is a toast; the tree that follows is drawn.
// - Events go one at a time: the next is sent when the tree answering the last arrives. Taps made
//   meanwhile wait in order; a deck swipe waiting already in the same direction isn't queued
//   again. After 1 s of waiting, "Working…" shows.
// - search/field: keys echo at once; `change` is sent 700 ms after the last key (and always before
//   any other event), `submit` on Enter (a multi-line field gets a new line). The blocks are laid
//   out above the keyboard, on the page that holds the focused input.
// - Each new tree redraws only the top-level blocks that changed (diffTrees picks the waveforms).
// - Back always works: the screen closes at once and the plugin hears `back` afterwards.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "plugins/host.h"
#include "screens/plugin_blocks.h"
#include "screens/screen.h"

namespace zk {

// The plugin host as a plugin screen sees it.
class ScreenLink {
public:
    virtual ~ScreenLink() = default;
    virtual void send(uint64_t instance, nlohmann::json event) = 0;
    // The user left the screen (Back, or it went with its tab).
    virtual void left(uint64_t instance) = 0;
    // The next ScreenTree/ScreenError/ScreenActionError/ScreenClose for this screen, if any.
    virtual bool take(uint64_t instance, PluginHostEvent& out) = 0;
};

class PluginScreen final : public Screen {
public:
    static constexpr int64_t kPauseMs = 700;     // typing → `change`
    static constexpr int64_t kWorkingMs = 1000;  // waiting → "Working…"
    enum class State { Loading, Shown, Failed };

    PluginScreen(ScreenLink&, uint64_t instance, std::string pluginName, std::string title);

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    void onClose(Nav&) override;
    bool wantsKeyboard() const override { return !focus_.focused.empty(); }
    bool showsTabBar() const override { return false; }
    bool closesWhenHidden() const override { return true; }

    uint64_t instance() const { return instance_; }
    State state() const { return state_; }
    const std::string& errorText() const { return error_; }
    bool working() const { return working_; }
    size_t waiting() const { return queue_.size(); }  // events not sent yet
    bool sending() const { return acked_ < sent_; }   // an event is with the plugin
    const Laid* laid() const { return tree_ ? &laid_ : nullptr; }
    const FocusState& focus() const { return focus_; }
    Rect area(Nav&) const;  // where the blocks go (above the keyboard)
    static Rect workingRect();
    static Rect backButtonRect();  // the error state's Back button

private:
    void receive(const PluginHostEvent&, Nav&);
    void showTree(std::shared_ptr<const Block>, uint64_t requestId, Nav&);
    void relayout(Nav&);
    void enqueue(nlohmann::json event, Nav&);
    void flushChange();
    void pump(Nav&);
    void setFocus(const std::string& id, Nav&);
    void turnPage(int delta, Nav&);
    void leave(Nav&);
    void tapBlocks(Point, Nav&);
    int nameReserve() const;

    ScreenLink& link_;
    const uint64_t instance_;
    const std::string pluginName_, title_;
    State state_ = State::Loading;
    std::string error_;
    std::shared_ptr<const Block> tree_;
    Laid laid_;
    int page_ = 0;
    FocusState focus_;
    std::map<std::string, std::string> lastSent_;  // the text last sent in change/submit, by id
    std::string pendingChange_;                    // the input typed into since its last change
    int64_t lastKeyMs_ = 0;
    std::deque<nlohmann::json> queue_;
    uint64_t sent_ = 0, acked_ = 0;
    int64_t sentAtMs_ = 0;
    bool working_ = false;
    bool closedByPlugin_ = false, left_ = false;
};

}  // namespace zk
