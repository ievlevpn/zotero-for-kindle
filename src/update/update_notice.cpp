#include "update/update_notice.h"

#include "core/log.h"

namespace zk {

namespace {

constexpr int64_t kCheckEveryMs = 3600 * 1000LL;  // an hour: one small GET (GitHub allows 60 an hour)

int64_t metaMs(Library& lib, const char* key) {
    const std::string v = lib.metaValue(key);
    try {
        return v.empty() ? 0 : std::stoll(v);
    } catch (...) {
        return 0;
    }
}

}  // namespace

UpdateNotice::UpdateNotice(Updater& updater, std::function<int64_t()> wallMs)
    : updater_(updater), wallMs_(std::move(wallMs)) {
    serial_ = updater_.status().serial;
}

void UpdateNotice::onSyncFinished(Library& lib, bool ok) {
    if (!ok) return;
    using Phase = Updater::Status::Phase;
    const Updater::Status s = updater_.status();
    if (s.phase != Phase::Idle && s.phase != Phase::UpToDate && s.phase != Phase::Failed &&
        s.phase != Phase::Available && s.phase != Phase::NotInstallable)
        return;  // checking, or an update under way
    if (s.phase == Phase::Failed && s.mustRestart) return;
    const int64_t now = wallMs_();
    const int64_t checked = metaMs(lib, "update:checkedMs"), failed = metaMs(lib, "update:failedMs");
    if (now < checked || now < failed) {  // the clock went back: count from now
        lib.setMetaValue("update:checkedMs", std::to_string(now));
        lib.setMetaValue("update:failedMs", "");
        return;
    }
    if (checked && now - checked < kCheckEveryMs) return;
    if (failed && failed > checked && now - failed < kCheckEveryMs) return;
    log(LogLevel::Info, "update: the hourly check");
    updater_.check();
    if (alsoCheck_) alsoCheck_();  // the plugin catalog, with the app's own check
    serial_ = -1;  // note its answer even if the status looks the same
}

bool UpdateNotice::poll(Library& lib) {
    using Phase = Updater::Status::Phase;
    const Updater::Status s = updater_.status();
    if (s.serial == serial_ || s.phase == Phase::Checking) return false;
    serial_ = s.serial;
    const auto before = available(lib);
    cached_.reset();  // written below
    const std::string now = std::to_string(wallMs_());
    switch (s.phase) {
        case Phase::UpToDate:
            lib.setMetaValue("update:checkedMs", now);
            lib.setMetaValue("update:available", "");
            break;
        case Phase::Available:
        case Phase::NotInstallable:
            lib.setMetaValue("update:checkedMs", now);
            lib.setMetaValue("update:available", s.release.version.str());
            break;
        case Phase::Failed:
            if (s.release.version == Version{}) lib.setMetaValue("update:failedMs", now);  // the check itself
            break;
        default: return false;
    }
    return available(lib) != before;
}

std::optional<Version> UpdateNotice::pending(Library& lib) const {
    const auto v = parseVersion(lib.metaValue("update:available"));
    if (!v || !(*v > updater_.current())) return std::nullopt;
    return v;
}

std::optional<Version> UpdateNotice::available(Library& lib) const {
    if (cached_ && cachedFor_ == &lib) return *cached_;
    auto v = pending(lib);
    if (v && lib.metaValue("update:dismissed") == v->str()) v.reset();
    cached_ = v;
    cachedFor_ = &lib;
    return v;
}

void UpdateNotice::dismiss(Library& lib) {
    cached_.reset();
    if (const auto v = pending(lib)) lib.setMetaValue("update:dismissed", v->str());
}

}  // namespace zk
