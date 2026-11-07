#pragma once
// Where the app keeps its files (spec §7.7).

#include <string>

namespace zk {

struct Paths {
    std::string configDir;      // credentials (Kindle: root-only partition)
    std::string droppedConfig;  // config.json dropped over USB, imported then deleted; "" = none
    std::string dbPath;         // library database
    std::string filesDir;       // downloaded attachments, <filesDir>/<attachmentKey>/<name>
    std::string logDir;
    std::string appDir;        // the app's folder (updates replace its bin/lib/share); "" = no updates
    std::string launcherPath;  // the library launcher, Zotero.sh
    std::string configFile() const { return configDir + "/config.json"; }
};

Paths defaultPaths();

}  // namespace zk
