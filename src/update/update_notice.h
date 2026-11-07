#pragma once
// "Version X is available" on Home: a check at most once an hour, after a sync that worked (Wi-Fi
// is up then; nothing turns it on for this), failed or not. Nothing downloads.
// Kept in the library's meta, so the notice outlives a restart:
//   update:checkedMs  wall clock of the last check that answered (or of a manual one)
//   update:failedMs   wall clock of the last check that didn't
//   update:available  the newer version the last check found ("" when up to date)
//   update:dismissed  the version whose banner was closed with ×
// The UI thread only: poll() notes what the Updater's worker learned.

#include "store/library.h"
#include "update/updater.h"

#include <functional>
#include <optional>

namespace zk {

class UpdateNotice {
public:
    UpdateNotice(Updater&, std::function<int64_t()> wallMs);

    void onSyncFinished(Library&, bool ok);  // starts a check when one is due
    bool poll(Library&);                     // true when what the Updater found changed the notice
    std::optional<Version> available(Library&) const;  // for the banner: newer and not dismissed
    std::optional<Version> pending(Library&) const;    // newer, dismissed or not (Settings)
    void dismiss(Library&);

private:
    Updater& updater_;
    std::function<int64_t()> wallMs_;
    int serial_ = -1;  // the Updater status last noted
};

}  // namespace zk
