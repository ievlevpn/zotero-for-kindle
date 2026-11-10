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
    host_ = host;
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
