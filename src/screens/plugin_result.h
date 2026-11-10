#pragma once
// A plugin action's run and its result pop-up: a bottom sheet like the reader's link pop-up.
//
//   Explain with AI · Explain                                  1 / 2
//   The text the action returned, plain, a blank line between
//   paragraphs; paged with swipes when longer than the sheet.
//   [ Close ]  [ Save as note ]  [ … ]          (at most 2 of the plugin's)
//
// The first run shows nothing here: the caller shows "Working…" on its bar or row while
// running() and !shown(). The answer opens the pop-up (nothing returned: no pop-up, Closed); an
// error is "<Plugin> couldn't do that: <message>" in the same pop-up. A plugin button runs the
// action again with {action, text}; the pop-up says "Working…" meanwhile, and what comes back
// replaces it (nothing: it closes). Close stops a run still going. The owner calls tick() every
// tick and drops the object once it is Closed; the pop-up damages its own place when it changes
// or goes.

#include <cstdint>
#include <string>
#include <vector>

#include <cairo.h>
#include <nlohmann/json.hpp>

#include "core/geometry.h"
#include "screens/plugin_entries.h"
#include "ui/gesture.h"

namespace zk {

class Nav;

class PluginResult {
public:
    enum class Status { Running, Shown, Closed };

    // Starts `action` (kind "selectionAction" or "itemAction") on `target`; the sheet sits at the
    // bottom of `area`.
    PluginResult(PluginEntries&, PluginAction, std::string kind, nlohmann::ordered_json target, Rect area);
    // Only a message (a plugin screen's onEvent error): shown at once, with Close.
    PluginResult(std::string label, std::string text, Rect area);

    Status status() const;
    bool running() const { return run_ != 0; }
    bool shown() const { return shown_; }
    const PluginAction& action() const { return action_; }
    const std::string& text() const { return text_; }
    const std::vector<std::pair<std::string, std::string>>& buttons() const { return buttons_; }

    Status tick(Nav&);
    Status onGesture(const Gesture&, Nav&);
    // The owner goes (or drops it): a run still going is stopped.
    void stop();
    void draw(cairo_t*) const;

    Rect rect() const;  // empty while not shown
    Rect closeRect() const;
    Rect buttonRect(int i) const;  // the plugin's button i
    int page() const { return page_; }
    int pages() const { return static_cast<int>(starts_.size()); }

private:
    void layout();
    void close(Nav&);
    std::string label() const;
    std::string body() const;  // "Working…" during a re-run

    PluginEntries* entries_ = nullptr;
    PluginAction action_;
    std::string kind_;
    nlohmann::ordered_json target_;
    Rect area_;
    std::string label_;  // the message's
    uint64_t run_ = 0;
    bool shown_ = false, closed_ = false;
    std::string text_;
    std::vector<std::pair<std::string, std::string>> buttons_;
    // Laid out: the sheet's height, the text's line tops, the first line of each page.
    int h_ = 0;
    std::vector<int> tops_;
    std::vector<int> starts_{0};
    int page_ = 0;
};

}  // namespace zk
