#pragma once
// Loads an engine module at runtime: libzk-epub.so / libzk-pdf.so from $ZK_LIB_DIR, next to the
// executable, or in ../lib (the Kindle install keeps them in /mnt/us/zotero/lib).

#include <memory>

#include "docs/document.h"

namespace zk {

std::unique_ptr<DocumentEngine> loadEngine(DocType, const EngineEnv&, UserError*);

// The factory each module exports (extern "C", the module's only exported symbol).
using EngineCreateFn = DocumentEngine* (*)(const EngineEnv*);

}  // namespace zk
