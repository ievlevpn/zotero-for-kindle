#pragma once

#include <string>
#include <vector>

#include "ui/canvas.h"

namespace zk {

// Static Home screen (mockup C, "Hybrid") with sample content, for the Plan 1 demo.
struct HomeDemoItem {
    std::string type;  // "EPUB" or "PDF"
    std::string title;
    std::string byline;  // "Raissi et al. · 2019"
    std::string meta;    // "p. 4 of 22", "71%", "new"
};

struct HomeDemoData {
    std::string continueTitle = "Recursive Macroeconomic Theory";
    std::string continueAuthors = "Ljungqvist · Sargent";
    std::string continueChapter = "Ch. 3 · Dynamic Programming";
    double continueProgress = 0.42;
    std::string continueMeta = "42% · 18 highlights · last read yesterday";
    std::vector<HomeDemoItem> recent = {
        {"PDF", "Physics-informed neural networks", "Raissi et al. · 2019", "p. 4 of 22"},
        {"EPUB", "The Structure of Scientific Revolutions", "Kuhn · 1962", "71%"},
        {"EPUB", "Dynamic Economics", "Adda, Cooper · 2003", "new"},
    };
    std::vector<std::string> tabs = {"Home", "Library", "Collections", "Notes"};
    int activeTab = 0;
};

void drawHomeDemo(Canvas& canvas, const HomeDemoData& data);

// Rectangle of tab `index` in the bottom tab bar (for hit testing and tap feedback).
Rect homeDemoTabRect(const Canvas& canvas, int index, int tabCount);

}  // namespace zk
