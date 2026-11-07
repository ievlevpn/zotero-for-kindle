#pragma once
// User settings kept in the library database (meta key "settings", JSON).

#include "store/library.h"

namespace zk {

struct AppSettings {
    int syncIntervalMinutes = 10;  // 0 = off
    int flashEvery = 6;            // page turns between full refreshes (used by the reader, Plan 4)
    bool landscapeReading = false;  // turn the screen while a book is open (off by default)
    int twoColumns = -1;            // EPUB in landscape: -1 by screen (on when the page is ≥ 2000 px wide), 0 off, 1 on
};

AppSettings loadSettings(Library&);
void saveSettings(Library&, const AppSettings&);

}  // namespace zk
