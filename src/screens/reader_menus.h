#pragma once
// The reader's menus (spec §6.3 state 2): top bar and bottom panel over the page.
// (Filled in by the menus task; this is the minimal shape the reader needs.)

#include "docs/document.h"
#include "screens/screen.h"

namespace zk {

class ReaderScreen;

class ReaderMenus {
public:
    explicit ReaderMenus(ReaderScreen& r) : reader_(r) {}
    void open(Nav&) {}
    void close(Nav&) {}
    void draw(Canvas&) {}
    void onGesture(const Gesture&, Nav&);
    void onKey(const KeyEvent&, Nav&) {}
    void onTick(Nav&) {}
    void onPage(Nav&) {}
    void onSyncEvent(const SyncEvent&, Nav&) {}
    void onSearchResults(const std::vector<SearchHit>&, Nav&) {}

private:
    ReaderScreen& reader_;
};

}  // namespace zk
