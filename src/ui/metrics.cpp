#include "ui/metrics.h"

namespace zk {

namespace {
Metrics g_metrics;  // set once at start, on the UI thread, before any screen exists
}  // namespace

const Metrics& metrics() { return g_metrics; }
void setMetrics(const Metrics& m) { g_metrics = m; }

}  // namespace zk
