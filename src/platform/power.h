#pragma once
// Keeps the Kindle from going to sleep while the app does background work (sync, downloads),
// and tells whether the app's storage is still there.

#include "platform/lipc.h"

#include <mutex>
#include <string>

namespace zk {

class KeepAwake {
public:
    explicit KeepAwake(Lipc& lipc) : lipc_(lipc) {}
    void acquire();  // first holder sets preventScreenSaver 1
    void release();  // last holder sets it back to 0; extra releases are ignored

private:
    Lipc& lipc_;
    std::mutex mutex_;
    int holders_ = 0;
};

struct KeepAwakeScope {
    explicit KeepAwakeScope(KeepAwake& k) : keep(k) { keep.acquire(); }
    ~KeepAwakeScope() { keep.release(); }
    KeepAwakeScope(const KeepAwakeScope&) = delete;
    KeepAwakeScope& operator=(const KeepAwakeScope&) = delete;
    KeepAwake& keep;
};

// The directory exists, is writable and its filesystem answers (false once /mnt/us is gone).
bool storageAvailable(const std::string& dir);

}  // namespace zk
