#pragma once
// The app's clipboard: text copied from a page (the web snapshot selection bar's Copy). Kept
// for the session, in this process only; the Kindle has no system clipboard to share it with.

#include <string>

namespace zk {

inline std::string& clipboardText() {
    static std::string text;
    return text;
}

}  // namespace zk
