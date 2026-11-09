#include "plugins/safe_mode.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "plugins/manifest.h"
#include "plugins/state.h"
#include "update/version.h"

namespace zk {

namespace fs = std::filesystem;

namespace {

int readStarts(const std::string& file) {
    std::ifstream in(file);
    int n = 0;
    if (!(in >> n) || n < 0) return 0;  // missing or garbled: no unconfirmed starts
    return n > 1000 ? 1000 : n;
}

// Write-then-rename, with the data on disk before it matters: a power cut or a kill right after
// must still leave the count.
void writeStarts(const std::string& dir, int n) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string file = dir + "/.starts", tmp = file + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const std::string text = std::to_string(n) + "\n";
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::fsync(fd);
    ::close(fd);
    if (ok) fs::rename(tmp, file, ec);
    else fs::remove(tmp, ec);
}

}  // namespace

SafeModeDecision decideSafeMode(const std::string& pluginsDir, bool launcherAskedNoPlugins) {
    std::error_code ec;
    if (launcherAskedNoPlugins || fs::exists(fs::path(pluginsDir) / "OFF", ec)) return {true, false};
    const int n = readStarts(pluginsDir + "/.starts");
    if (n < 2) {
        writeStarts(pluginsDir, n + 1);
        return {false, false};
    }
    PluginState state(pluginsDir + "/.state.json");
    for (const auto& f : scanPlugins(pluginsDir, appVersion())) state.turnOff(f.dir);
    writeStarts(pluginsDir, 0);
    return {true, true};
}

void confirmSafeModeStart(const std::string& pluginsDir) {
    std::error_code ec;
    if (!fs::exists(pluginsDir + "/.starts", ec)) return;
    writeStarts(pluginsDir, 0);
}

}  // namespace zk
