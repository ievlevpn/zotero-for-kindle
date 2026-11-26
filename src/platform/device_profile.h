#pragma once
// What a Kindle doesn't tell us about itself, by FBInk's device name (Plan 6). Unknown devices
// get FBInk's ppi (300 if it reports none) and no overrides: the measured window does the rest.

#include <string>

namespace zk {

struct DeviceProfile {
    std::string name;
    int ppi = 300;
    int keyboardHeightPx = 0;  // the system keyboard, if lipc gives no bounds; 0: unknown
};

DeviceProfile profileFor(const std::string& fbinkName, int fbinkPpi);

// Page content (Waveform::Quality) with REAGL, the Kindle reader's own page-turn waveform: no
// flash, less ghosting and quicker to look settled than GC16. On the MediaTek Kindles (PW5 and
// later, the Scribes, the Colorsoft), where FBInk maps it to the driver's GLR16. `env`: ZK_REAGL
// (nullptr when unset), "0" turns it off, "1" on elsewhere too.
bool pageReagl(bool mtk, const char* env);

}  // namespace zk
