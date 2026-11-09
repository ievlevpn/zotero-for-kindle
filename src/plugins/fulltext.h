#pragma once
// The full text of a downloaded book or paper, for plugins (zk.library.fullText, Task 10).
//
// Runs on the plugin thread. It opens its own engine instance (the reader's belongs to the screen
// thread); crengine's process-wide lock serialises it with the reader, held per chapter only.
//
// Format of the text (also in zk.d.ts):
//  - EPUB: paragraphs separated by one blank line ("\n\n"), chapters in reading order, no other
//    layout (no page breaks: an EPUB has no fixed pages).
//  - PDF: the text of each page in order, pages separated by a form feed ("\f") so a plugin can
//    split them (text.split("\f")[n] is page n+1); lines within a page end in "\n".
// Limit: kMaxFullTextBytes (32 MB) of text; a larger file is an error, never a truncation.
//
// Cache: <pluginsDir>/.cache/fulltext/<attachmentKey>-<md5>.txt, written atomically (temp file,
// then rename). A hit doesn't open the engine. Writing a new entry removes the attachment's older
// entries (same key, another md5).

#include <optional>
#include <string>

#include "docs/document.h"

namespace zk {

// nullopt with *err set: the file isn't on the Kindle (not downloaded, or its file is gone), it
// is a linked file or not an EPUB/PDF, the engine can't open it, or its text is over the limit.
std::optional<std::string> extractFullText(const AttachmentRecord&, const std::string& pluginsDir, const EngineEnv&,
                                           UserError* err);

}  // namespace zk
