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
};

Paths defaultPaths();

}  // namespace zk
