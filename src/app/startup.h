#pragma once
// Which account the app starts with: a config.json dropped over USB is imported (and removed)
// first, then the private config is read. No config -> the Setup screen.

#include <optional>

#include "app/paths.h"
#include "sync/config.h"

namespace zk {

struct Startup {
    std::optional<Config> config;
    bool imported = false;  // came from a dropped config.json just now
};

Startup startupConfig(const Paths&);

}  // namespace zk
