// libzk-pdf.so's only exported symbol.
#include "docs/engine_factory.h"
#include "docs/guarded_engine.h"
#include "docs/pdf/pdf_engine.h"

extern "C" __attribute__((visibility("default"))) zk::DocumentEngine* zk_pdf_engine_create(const zk::EngineEnv*) {
    try {
        return new zk::GuardedEngine(std::make_unique<zk::PdfEngine>());
    } catch (...) {
        return nullptr;
    }
}
