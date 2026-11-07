#include "platform/power.h"

#include <sys/statvfs.h>
#include <unistd.h>

namespace zk {

void KeepAwake::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (holders_++ == 0) lipc_.set("com.lab126.powerd", "preventScreenSaver", "1");
}

void KeepAwake::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (holders_ == 0) return;
    if (--holders_ == 0) lipc_.set("com.lab126.powerd", "preventScreenSaver", "0");
}

bool storageAvailable(const std::string& dir) {
    struct statvfs st {};
    return ::access(dir.c_str(), W_OK) == 0 && ::statvfs(dir.c_str(), &st) == 0;
}

}  // namespace zk
