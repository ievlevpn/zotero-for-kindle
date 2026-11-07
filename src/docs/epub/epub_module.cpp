// libzk-epub.so's only exported symbol.
#include "docs/engine_factory.h"
#include "docs/guarded_engine.h"
#include "docs/epub/epub_engine.h"

extern "C" __attribute__((visibility("default"))) zk::DocumentEngine* zk_epub_engine_create(const zk::EngineEnv* env) {
    try {
        return new zk::GuardedEngine(std::make_unique<zk::EpubEngine>(env ? *env : zk::EngineEnv{}));
    } catch (...) {
        return nullptr;  // nothing crosses the module boundary as an exception
    }
}
