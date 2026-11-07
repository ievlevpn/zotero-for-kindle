#pragma once
// A screen of the app and the navigation it can ask for. Screens draw the whole window
// (the app adds the tab bar), react to gestures, keys and sync events, and report which
// parts of the window changed with which e-ink waveform.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "platform/display.h"
#include "platform/input.h"
#include "platform/keyboard.h"
#include "store/library.h"
#include "sync/sync_control.h"
#include "ui/canvas.h"
#include "ui/gesture.h"

namespace zk {

class Screen;

class Nav {
public:
    virtual ~Nav() = default;
    virtual void push(std::unique_ptr<Screen>) = 0;
    virtual void pop() = 0;
    virtual void switchTab(int index) = 0;
    virtual void damage(Rect, Waveform) = 0;  // ignored unless the caller is the visible screen
    virtual void redrawAll(Waveform) = 0;
    virtual SyncControl& sync() = 0;
    virtual Library& library() = 0;
    virtual Keyboard& keyboard() = 0;
    virtual int64_t nowMs() const = 0;
    virtual void quit() = 0;
    virtual void restart() { quit(); }  // quit with exit code 75: Zotero.sh starts the app again (an update)
    virtual void toast(const std::string& text) = 0;  // a short message above the tab bar, ~3 s
    virtual const Screen* top() const = 0;            // the visible screen
    virtual void orientationChanged() {}               // the landscape setting changed: turn if needed
};

// What a list row shows instead of its byline (download progress or an error), by attachment key.
using StatusFn = std::function<std::string(const std::string& attachmentKey)>;

class Screen {
public:
    virtual ~Screen() = default;
    virtual void draw(Canvas&) = 0;
    virtual void onGesture(const Gesture&, Nav&) = 0;
    virtual void onKey(const KeyEvent&, Nav&) {}
    virtual void onSyncEvent(const SyncEvent&, Nav&) {}
    virtual void onShow(Nav&) {}
    virtual void onTick(Nav&) {}  // every loop iteration while visible (timers)
    // Part of reading (the reader and its panels): turns to landscape when the user chose it.
    virtual bool isReading() const { return false; }
    // The window changed size (turned): lay out again. Most screens just do what showing does.
    virtual void onResize(Nav& nav) { onShow(nav); }
    virtual bool wantsKeyboard() const { return false; }
    virtual bool showsTabBar() const { return true; }
};

}  // namespace zk
