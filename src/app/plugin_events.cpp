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
    host_ = host;
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
            case PluginHostEvent::Kind::Badge:
                badges_[ev.pluginId] = ev.text;
                break;
            case PluginHostEvent::Kind::Disabled:
            case PluginHostEvent::Kind::Log:
                if (ev.kind == PluginHostEvent::Kind::Disabled)
                    log(LogLevel::Warn, "plugin %s: %s", ev.pluginId.c_str(), ev.text.c_str());
                notices_.push_back({ev.kind, ev.pluginId, ev.pluginName, ev.text});
                if (notices_.size() > kNotices) notices_.erase(notices_.begin());
                break;
        }
    }
}

}  // namespace zk
