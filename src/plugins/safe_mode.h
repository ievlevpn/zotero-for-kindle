#pragma once
// Safe mode: plugins never keep the app from starting. Three ways in, none needing a reinstall:
//   automatic    two starts in a row with a plugin on never got to 60 s of running (or a clean exit):
//                every plugin is turned off in PluginState; turning one back on is a deliberate act
//                in Settings → Plugins
//   OFF file     <pluginsDir>/OFF (any content): plugins are off while it exists (nothing persisted)
//   launcher     ZK_NO_PLUGINS=1, set by documents/Zotero-no-plugins.sh: off for that run only
// In every case the PluginHost is not started: no plugin code runs.

#include <csignal>
#include <cstring>
#include <string>

#include <unistd.h>

namespace zk {

struct SafeModeDecision {
    bool pluginsOff = false;
    bool automatic = false;  // only the automatic case; it alone gets a notice on Home
    // This start was counted (some installed plugin is on): confirm it after 60 s, at a clean exit
    // or from the ending signal. With no plugin on nothing is counted, so nothing is confirmed.
    bool counting = false;
};

// Decides at start. With some installed plugin on it counts this start in <pluginsDir>/.starts
// (written durably before plugins start); the third consecutive start that was never confirmed
// turns the plugins that are on off automatically and resets the count. With none on, nothing is
// counted or written (a count left from before is dropped). Off for another reason: the count is
// left alone.
SafeModeDecision decideSafeMode(const std::string& pluginsDir, bool launcherAskedNoPlugins);

// The start is confirmed (60 s of running, or a clean exit): the count goes back to 0.
void confirmSafeModeStart(const std::string& pluginsDir);

// The system ending the app (SIGTERM, SIGINT, SIGHUP) is a clean end, not a crash. The path is kept
// here when armed, so the signal path needs no allocation: confirmFromSignal() only unlinks
// <pluginsDir>/.starts (async-signal-safe; a missing file reads as a count of 0). Header-only so the
// wet-ink handler (another library) can call it too.
namespace safe_mode_detail {
inline char gStartsPath[512] = {0};
}
inline void armSafeModeSignalConfirm(const std::string& pluginsDir) {
    const std::string path = pluginsDir + "/.starts";
    if (path.size() < sizeof safe_mode_detail::gStartsPath) std::memcpy(safe_mode_detail::gStartsPath, path.c_str(), path.size() + 1);
}
inline void confirmSafeModeStartFromSignal() {
    if (safe_mode_detail::gStartsPath[0]) ::unlink(safe_mode_detail::gStartsPath);
}
// Handler for SIGTERM/SIGINT/SIGHUP: confirm, then die of the signal as it would have.
inline void safeModeEndingSignal(int sig) {
    confirmSafeModeStartFromSignal();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// How long a start runs before it counts as confirmed.
constexpr int kSafeModeConfirmMs = 60'000;

}  // namespace zk
