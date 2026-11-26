#include "platform/device_profile.h"

namespace zk {

DeviceProfile profileFor(const std::string& fbinkName, int fbinkPpi) {
    DeviceProfile p{fbinkName, fbinkPpi > 0 ? fbinkPpi : 300, 0};
    // Measured with the keyboard open (lipc com.lab126.keyboard bounds), 2026-11-07.
    if (fbinkName == "PaperWhite 5") p.keyboardHeightPx = 567;
    if (fbinkName == "Scribe") p.keyboardHeightPx = 808;
    // Kaleido 3: 300 ppi black and white under a 150 ppi color filter; the app lays out at 300.
    if (fbinkName == "ColorSoft" || fbinkName == "Scribe ColorSoft" || fbinkName == "ColorSoft 2") p.ppi = 300;
    return p;
}

bool pageReagl(bool mtk, const char* env) {
    if (env && std::string(env) == "0") return false;
    if (env && std::string(env) == "1") return true;
    return mtk;
}

}  // namespace zk
