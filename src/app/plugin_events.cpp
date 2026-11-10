#include "app/plugin_events.h"

#include "app/app.h"
#include "core/log.h"

namespace zk {

void forwardSyncToPlugins(const SyncEvent& e, PluginSink& sink) {
    if (e.kind != SyncEvent::Kind::SyncFinished && e.kind != SyncEvent::Kind::PushFinished) return;
    for (const PluginEvent& pe : e.report.pluginEvents) sink.emit(pe);
    if (e.kind == SyncEvent::Kind::SyncFinished && !e.error)
        sink.emit(PluginEvent{"syncFinished", {{"origin", "zotero"}, {"initial", e.report.initial}}, false});
}

PluginEvents::PluginEvents(PluginHost* host) : host_(host) {}

void PluginEvents::rebind(PluginHost* host, App& app) {
    for (uint64_t id : open_) app.withdrawConfirm(id);
    open_.clear();
    for (const auto& [instance, name] : screens_) {  // that host's screens are gone with it
        PluginHostEvent ev{PluginHostEvent::Kind::ScreenError, "", name, name + " can't show this screen right now", {}, 0, ""};
        ev.instance = instance;
        mail_[instance].push_back(std::move(ev));
    }
    screens_.clear();  // nothing of them goes to the next host (take() still hands over the error)
    for (const auto& [run, name] : runs_) outcomes_[run] = ActionOutcome{true, false, name + " couldn't do that: it stopped", {}};
    runs_.clear();
    host_ = host;
    woken_ = false;
    ++rebinds_;
}

std::vector<PluginStatus> PluginEvents::running() {
    if (!host_ || !running_) return {};
    std::vector<PluginStatus> all = host_->status(), out;
    bool asleep = false;
    for (auto& s : all) {
        if (!s.enabled || s.stopped || !s.folder.manifest || !s.folder.problem.empty()) continue;
        if (!s.loaded) asleep = true;
        else out.push_back(std::move(s));
    }
    if (asleep && !woken_) {
        woken_ = true;
        host_->wake();
    }
    return out;
}

std::vector<PluginAction> PluginEvents::actions(const std::string& kind) {
    std::vector<PluginAction> out;
    for (const auto& s : running())
        for (const auto& e : s.entries)
            if (e.kind == kind) out.push_back({s.folder.dir, s.folder.manifest->name, e.id, e.title});
    return out;
}

std::vector<PluginLaunch> PluginEvents::mainScreens() {
    std::vector<PluginLaunch> out;
    for (const auto& s : running())
        for (const auto& e : s.entries)
            if (e.kind == "screen" && e.main) out.push_back({s.folder.dir, s.folder.manifest->name, e.id, e.title, s.badge});
    return out;
}

bool PluginEvents::anyBadge() {
    for (const auto& s : running())
        if (!s.badge.empty()) return true;
    return false;
}

uint64_t PluginEvents::version() {
    // Plugins starting or stopping to run, or another host, change it too.
    return ((host_ ? host_->entriesVersion() : 0) << 8) + (rebinds_ << 1) + (running_ ? 1 : 0);
}

uint64_t PluginEvents::run(const PluginAction& a, const std::string& kind, nlohmann::ordered_json target, nlohmann::json again) {
    if (!host_ || !running_) {
        static uint64_t offline = 0;
        const uint64_t run = (uint64_t{1} << 63) + ++offline;  // never a host's
        outcomes_[run] = ActionOutcome{true, false, a.pluginName + " couldn't do that: plugins aren't running", {}};
        return run;
    }
    const uint64_t run = host_->runAction(a.pluginId, kind, a.id, std::move(target), std::move(again));
    runs_[run] = a.pluginName;
    return run;
}

std::optional<ActionOutcome> PluginEvents::take(uint64_t run) {
    auto it = outcomes_.find(run);
    if (it == outcomes_.end()) return std::nullopt;
    ActionOutcome out = std::move(it->second);
    outcomes_.erase(it);
    return out;
}

void PluginEvents::stop(uint64_t run) {
    outcomes_.erase(run);
    if (runs_.erase(run) && host_) host_->stopAction(run);
}

void PluginEvents::openScreen(const std::string& pluginId, const std::string& screenId) {
    if (app_) openScreen(*app_, pluginId, screenId);
}

void PluginEvents::pushScreen(App& app, uint64_t instance, const std::string& pluginName, const std::string& title) {
    screens_[instance] = pluginName;
    app.push(std::make_unique<PluginScreen>(*this, instance, pluginName, title));
}

uint64_t PluginEvents::openScreen(App& app, const std::string& pluginId, const std::string& screenId, nlohmann::json params) {
    std::string name = pluginId, title;
    if (host_)
        for (const auto& st : host_->status()) {
            if (st.folder.dir != pluginId) continue;
            if (st.folder.manifest) name = st.folder.manifest->name;
            for (const auto& e : st.entries)
                if (e.kind == "screen" && e.id == screenId) title = e.title;
        }
    if (!host_ || !running_) {  // nothing would ever answer: the error state at once
        static uint64_t offline = 0;
        const uint64_t instance = (uint64_t{1} << 63) + ++offline;  // never a host's
        PluginHostEvent ev{PluginHostEvent::Kind::ScreenError, pluginId, name, name + " can't show this screen right now", {}, 0, ""};
        ev.instance = instance;
        mail_[instance].push_back(std::move(ev));
        app.push(std::make_unique<PluginScreen>(*this, instance, name, title));
        return instance;
    }
    const uint64_t instance = host_->openScreen(pluginId, screenId, std::move(params));
    pushScreen(app, instance, name, title);
    return instance;
}

void PluginEvents::send(uint64_t instance, nlohmann::json event) {
    if (host_ && screens_.count(instance)) host_->screenEvent(instance, std::move(event));
}

void PluginEvents::left(uint64_t instance) {
    screens_.erase(instance);
    mail_.erase(instance);
    if (host_) host_->closeScreen(instance);
}

bool PluginEvents::take(uint64_t instance, PluginHostEvent& out) {
    auto it = mail_.find(instance);
    if (it == mail_.end() || it->second.empty()) return false;
    out = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty() && !screens_.count(instance)) mail_.erase(it);  // closed by the plugin: its last word
    return true;
}

void PluginEvents::poll(App& app) {
    if (!host_) return;
    PluginHostEvent ev;
    while (host_->poll(ev)) {
        switch (ev.kind) {
            case PluginHostEvent::Kind::Confirm: {
                std::vector<std::string> lines{ev.text};
                lines.insert(lines.end(), ev.lines.begin(), ev.lines.end());
                const uint64_t id = ev.requestId;
                open_.insert(id);
                log(LogLevel::Info, "plugin %s asks: %s", ev.pluginId.c_str(), ev.text.c_str());
                app.confirm(id, PluginConfirm(ev.pluginName, std::move(lines), ev.action), [this, id](bool yes) {
                    if (!open_.erase(id) || !host_) return;  // withdrawn meanwhile: nobody waits for it
                    host_->answer(id, yes);
                });
                break;
            }
            case PluginHostEvent::Kind::Withdrawn:
                if (open_.erase(ev.requestId)) app.withdrawConfirm(ev.requestId);
                break;
            case PluginHostEvent::Kind::ChangesQueued:
                app.sync().pushSoon();
                break;
            case PluginHostEvent::Kind::BackgroundDone:
                if (backgroundDone_) backgroundDone_(ev.requestId);
                break;
            case PluginHostEvent::Kind::Disabled:
                log(LogLevel::Warn, "plugin %s: %s", ev.pluginId.c_str(), ev.text.c_str());
                turnedOff_.push_back(ev.text);
                break;
            case PluginHostEvent::Kind::Badge:
            case PluginHostEvent::Kind::Log:
                break;
            case PluginHostEvent::Kind::ScreenOpen:
                if (ev.parent != 0 && !screens_.count(ev.parent)) host_->closeScreen(ev.instance);  // its parent went
                else pushScreen(app, ev.instance, ev.pluginName, ev.text);
                break;
            case PluginHostEvent::Kind::ScreenClose:
                if (screens_.erase(ev.instance)) mail_[ev.instance].push_back(std::move(ev));  // the screen pops itself
                break;
            case PluginHostEvent::Kind::ActionResult:
            case PluginHostEvent::Kind::ActionError: {
                if (!runs_.erase(ev.requestId)) break;  // stopped meanwhile
                ActionOutcome out;
                out.error = ev.kind == PluginHostEvent::Kind::ActionError;
                if (out.error) out.text = ev.text;
                else if (ev.value.is_null()) out.none = true;
                else {
                    out.text = ev.value.value("text", "");
                    for (const auto& b : ev.value.value("actions", nlohmann::json::array()))
                        out.buttons.emplace_back(b.value("id", ""), b.value("title", ""));
                }
                outcomes_[ev.requestId] = std::move(out);
                break;
            }
            case PluginHostEvent::Kind::ScreenTree:
            case PluginHostEvent::Kind::ScreenError:
            case PluginHostEvent::Kind::ScreenActionError:
                if (screens_.count(ev.instance)) mail_[ev.instance].push_back(std::move(ev));
                break;
        }
    }
    // Not over a system dialog or the screensaver: shown when the app is in front again.
    if (!turnedOff_.empty() && !app.inBackground() && app.nowMs() - lastToastMs_ >= kToastGapMs) {
        app.toast(turnedOff_.front());
        turnedOff_.pop_front();
        lastToastMs_ = app.nowMs();
    }
}

}  // namespace zk
