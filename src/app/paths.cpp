#include "app/paths.h"

namespace zk {

Paths defaultPaths() {
#ifdef ZK_KINDLE
    return {"/var/local/zotero", "/mnt/us/zotero/config.json", "/mnt/us/zotero/zotero.db", "/mnt/us/zotero/files",
            "/mnt/us/zotero/log", "/mnt/us/zotero", "/mnt/us/documents/Zotero.sh", "/mnt/us/zotero/plugins"};
#else
    return {"local", "", "local/zk.sqlite", "local/files", "local", "", "", "local/plugins"};
#endif
}

}  // namespace zk
