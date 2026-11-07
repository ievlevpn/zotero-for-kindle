#pragma once
// A screen of the app and the navigation it can ask for. Screens draw the whole window
// (the app adds the tab bar), react to gestures, keys and sync events, and report which
// parts of the window changed with which e-ink waveform.

#include <cstdint>
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
};

class Screen {
public:
    virtual ~Screen() = default;
    virtual void draw(Canvas&) = 0;
    virtual void onGesture(const Gesture&, Nav&) = 0;
    virtual void onKey(const KeyEvent&, Nav&) {}
    virtual void onSyncEvent(const SyncEvent&, Nav&) {}
    virtual void onShow(Nav&) {}
    virtual void onTick(Nav&) {}  // every loop iteration while visible (timers)
    virtual bool wantsKeyboard() const { return false; }
    virtual bool showsTabBar() const { return true; }
};

}  // namespace zk
