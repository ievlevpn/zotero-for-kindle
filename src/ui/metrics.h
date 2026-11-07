#pragma once
// The window the app lays out in (Plan 6): its size and origin on the screen, measured from X at
// start, and the device's ppi and name. Layout lengths are pixels at 300 ppi (11.81 px = 1 mm),
// the same physical size on every 300-ppi Kindle; nothing may assume a particular window size.

#include <string>

#include "core/geometry.h"

namespace zk {

struct Metrics {
    int width = 1236, height = 1547;
    Point origin{0, 101};
    int ppi = 300;
    std::string device = "PaperWhite 5";  // FBInk's name
    Rect window() const { return {0, 0, width, height}; }
};

inline const Metrics kPw5{1236, 1547, {0, 101}, 300, "PaperWhite 5"};
inline const Metrics kScribe{1860, 2379, {0, 101}, 300, "Scribe"};

const Metrics& metrics();
void setMetrics(const Metrics&);

// For tests: these metrics until the end of the scope.
class ScopedMetrics {
public:
    explicit ScopedMetrics(const Metrics& m) : saved_(metrics()) { setMetrics(m); }
    ~ScopedMetrics() { setMetrics(saved_); }
    ScopedMetrics(const ScopedMetrics&) = delete;
    ScopedMetrics& operator=(const ScopedMetrics&) = delete;

private:
    Metrics saved_;
};

}  // namespace zk
