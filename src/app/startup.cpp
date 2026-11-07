#include "app/startup.h"

namespace zk {

Startup startupConfig(const Paths& p) {
    Startup s;
    if (!p.droppedConfig.empty()) s.imported = importDroppedConfig(p.droppedConfig, p.configDir);
    s.config = loadConfig(p.configDir);
    return s;
}

}  // namespace zk
