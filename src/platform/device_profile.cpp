#include "platform/device_profile.h"

namespace zk {

DeviceProfile profileFor(const std::string& fbinkName, int fbinkPpi) {
    DeviceProfile p{fbinkName, fbinkPpi > 0 ? fbinkPpi : 300, 0};
    // Measured with the keyboard open (lipc com.lab126.keyboard bounds), 2026-11-07.
    if (fbinkName == "PaperWhite 5") p.keyboardHeightPx = 567;
    if (fbinkName == "Scribe") p.keyboardHeightPx = 808;
    return p;
}

}  // namespace zk
