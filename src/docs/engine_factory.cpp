#include "docs/engine_factory.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "core/log.h"

namespace zk {

namespace fs = std::filesystem;

namespace {

fs::path exeDir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::path(buf).parent_path();
    return ".";
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path(".") : p.parent_path();
#endif
}

}  // namespace

std::unique_ptr<DocumentEngine> loadEngine(DocType type, const EngineEnv& env, UserError* err) {
    const char* name = type == DocType::Epub ? "libzk-epub.so" : "libzk-pdf.so";
    const char* symbol = type == DocType::Epub ? "zk_epub_engine_create" : "zk_pdf_engine_create";
    std::vector<fs::path> dirs;
    if (const char* d = std::getenv("ZK_LIB_DIR")) dirs.emplace_back(d);
    const fs::path exe = exeDir();
    dirs.push_back(exe);
    dirs.push_back(exe / ".." / "lib");
    dirs.push_back(exe / "..");
    std::string why = std::string(name) + " not found";
    for (const auto& d : dirs) {
        const fs::path p = d / name;
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        // Loaded once and kept for the process lifetime (engines may be created again).
        void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            why = dlerror();
            continue;
        }
        auto create = reinterpret_cast<EngineCreateFn>(dlsym(h, symbol));
        if (!create) {
            why = std::string(symbol) + " missing in " + p.string();
            continue;
        }
        if (DocumentEngine* e = create(&env)) return std::unique_ptr<DocumentEngine>(e);
        why = std::string(symbol) + " returned nothing";
    }
    log(LogLevel::Error, "engine: %s", why.c_str());
    if (err)
        *err = UserError{ErrorKind::Other,
                         type == DocType::Epub ? "The EPUB reader is missing from this install."
                                               : "The PDF reader is missing from this install.",
                         why};
    return nullptr;
}

}  // namespace zk
