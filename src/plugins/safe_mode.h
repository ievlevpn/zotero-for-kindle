#pragma once
// Safe mode: plugins never keep the app from starting. Three ways in, none needing a reinstall:
//   automatic    two starts in a row with plugins on never got to 60 s of running (or a clean exit):
//                every plugin is turned off in PluginState; turning one back on is a deliberate act
//                in Settings → Plugins
//   OFF file     <pluginsDir>/OFF (any content): plugins are off while it exists (nothing persisted)
//   launcher     ZK_NO_PLUGINS=1, set by documents/Zotero-no-plugins.sh: off for that run only
// In every case the PluginHost is not started: no plugin code runs.

#include <string>

namespace zk {

struct SafeModeDecision {
    bool pluginsOff = false;
    bool automatic = false;  // only the automatic case; it alone gets a notice on Home
};

// Decides at start. With plugins on it counts this start in <pluginsDir>/.starts (written durably
// before plugins start); the third consecutive start that was never confirmed turns plugins off
// automatically and resets the count. Off for another reason: the count is left alone.
SafeModeDecision decideSafeMode(const std::string& pluginsDir, bool launcherAskedNoPlugins);

// The start is confirmed (60 s of running, or a clean exit): the count goes back to 0.
void confirmSafeModeStart(const std::string& pluginsDir);

// How long a start runs before it counts as confirmed.
constexpr int kSafeModeConfirmMs = 60'000;

}  // namespace zk
