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

}  // namespace zk
