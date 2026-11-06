#include "core/log.h"

int main() {
    zk::log(zk::LogLevel::Info, "zotero starting (backend=%s)", ZK_BACKEND);
    return 0;
}
