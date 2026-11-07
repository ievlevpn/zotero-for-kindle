#include "app/settings_store.h"

#include <nlohmann/json.hpp>

namespace zk {

AppSettings loadSettings(Library& lib) {
    AppSettings s;
    auto j = nlohmann::json::parse(lib.metaValue("settings"), nullptr, false);
    if (!j.is_object()) return s;
    if (j.contains("syncIntervalMinutes") && j["syncIntervalMinutes"].is_number_integer())
        s.syncIntervalMinutes = j["syncIntervalMinutes"].get<int>();
    if (j.contains("flashEvery") && j["flashEvery"].is_number_integer()) s.flashEvery = j["flashEvery"].get<int>();
    return s;
}

void saveSettings(Library& lib, const AppSettings& s) {
    lib.setMetaValue("settings",
                     nlohmann::json{{"syncIntervalMinutes", s.syncIntervalMinutes}, {"flashEvery", s.flashEvery}}.dump());
}

}  // namespace zk
