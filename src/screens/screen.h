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
#include "plugins/events.h"
#include "store/library.h"
#include "sync/sync_control.h"
#include "ui/canvas.h"
#include "ui/gesture.h"

namespace zk {

class Screen;
class PluginEntries;  // plugin_entries.h
class CatalogService;  // app/catalog_service.h

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
    // Draw now and show it at once (pen ink: no full-screen redraw per sample). The drawing must
    // also come out of the screen's draw() later, which may repaint the area at any time.
    virtual void paintNow(Rect r, Waveform w, const std::function<void(cairo_t*)>&) { damage(r, w); }
    // Whether the system can draw the pen's line itself, and asking it to (only while the asking
    // screen is visible; the app turns it off whenever another screen shows).
    virtual bool systemInk() const { return false; }
    virtual void setSystemInk(bool /*on*/) {}
    // Where plugin events go (src/plugins/events.h): queuing only, never waits. Drops them by default.
    virtual PluginSink& plugins() { return nullPluginSink(); }
    // Plugins' entry points and actions (plugin_entries.h); null: none (no plugin shows anywhere).
    virtual PluginEntries* pluginEntries() { return nullptr; }
    // The plugin catalog (Get plugins, updates); null: none (the screens show nothing of it).
    virtual CatalogService* catalog() { return nullptr; }
    // A timer of the caller's (a pause after typing, a delayed "Working…"): the loop runs again,
    // with an onTick, by `ms` (this clock) at the latest. The loop otherwise sleeps until input or a
    // worker's wake (core/wake.h), so a screen whose onTick waits for time asks on every tick
    // while it does. Results from worker threads need no timer: their wake runs the loop.
    virtual void wakeAt(int64_t /*ms*/) {}
};

// What a list row shows instead of its byline (download progress or an error), by attachment key.
using StatusFn = std::function<std::string(const std::string& attachmentKey)>;

class Screen {
public:
    virtual ~Screen() = default;
    virtual void draw(Canvas&) = 0;
    virtual void onGesture(const Gesture&, Nav&) = 0;
    virtual void onKey(const KeyEvent&, Nav&) {}
    // The stylus (Scribe), in window pixels. Returning false for a Down makes that stroke a finger
    // touch instead (taps, swipes): screens that don't write with the pen needn't do anything.
    virtual bool onPen(const PenEvent&, Nav&) { return false; }
    virtual void onSyncEvent(const SyncEvent&, Nav&) {}
    virtual void onShow(Nav&) {}
    // Every loop iteration while visible. The loop runs on input, on a worker's wake and at the
    // times asked with Nav::wakeAt, not on its own: a timer here must ask for its time.
    virtual void onTick(Nav&) {}
    // The Scribe's wet ink may draw over this screen (the reader on a PDF page taking handwriting).
    virtual bool wantsSystemInk() const { return false; }
    // Another screen now shows over this one (or another tab): it stays alive, but gets no ticks.
    virtual void onHide(Nav&) {}
    // The app went to the background (a system dialog, the screensaver), this screen visible.
    virtual void onBackground(Nav&) {}
    // Just before the screen goes (Back, a new root, the app closing): finish what must not be
    // lost. May take a moment (bounded); the screen is still where it was.
    virtual void onClose(Nav&) {}
    // Part of reading (the reader and its panels): turns to landscape when the user chose it.
    virtual bool isReading() const { return false; }
    // A page turn is under way (the reader): the app's own pop-ups wait until it has landed.
    virtual bool midTurn() const { return false; }
    // The window changed size (turned): lay out again. Most screens just do what showing does.
    virtual void onResize(Nav& nav) { onShow(nav); }
    virtual bool wantsKeyboard() const { return false; }
    // A finger moving past the slop drags at once, without a long press first (Mark an area).
    virtual bool dragsAtOnce() const { return false; }
    // The same for a touch going down at `at` (the reader's page slider): asked on every Down.
    virtual bool dragsAtOnceFrom(Point /*at*/) const { return dragsAtOnce(); }
    // Its Fast damages are shown one by one, not merged into the box around them (a moving frame).
    virtual bool fastPartsApart() const { return false; }
    virtual bool showsTabBar() const { return true; }
    // Closed (with what is above it) when its tab is left, instead of kept hidden (plugin screens).
    virtual bool closesWhenHidden() const { return false; }
};

}  // namespace zk
