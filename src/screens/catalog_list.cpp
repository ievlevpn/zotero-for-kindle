#include "screens/catalog_list.h"

#include <algorithm>
#include <ctime>

#include <glib.h>

#include "app/catalog_service.h"
#include "screens/catalog_page.h"
#include "screens/plugin_page.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }
constexpr int kCatalogRowH = 160;
constexpr int kTop = kHeaderH + 10;
constexpr int kLineH = 90;  // "As of 9 Nov" (and a failure) above the list

std::string folded(const std::string& s) {
    gchar* f = g_utf8_casefold(s.c_str(), -1);
    std::string out = f ? f : s;
    g_free(f);
    return out;
}

}  // namespace

std::string asOf(int64_t wallMs) {
    if (wallMs <= 0) return "";
    const std::time_t t = static_cast<std::time_t>(wallMs / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return "As of " + std::to_string(tm.tm_mday) + " " + kMonths[tm.tm_mon % 12];
}

CatalogListScreen::CatalogListScreen(PluginHostFn host) : host_(std::move(host)) {}

int CatalogListScreen::perPage() const { return std::max(1, (H() - kTop - kLineH - kPageIndicatorH) / kCatalogRowH); }

void CatalogListScreen::load() {
    all_.clear();
    if (!catalog_) return;
    const auto& st = catalog_->state();
    serial_ = st.serial;
    if (!st.cache.registry) {
        pager_ = Pager(0, perPage());
        return;
    }
    PluginHost* h = host_ ? host_() : nullptr;
    const auto installed = h ? h->status() : std::vector<PluginStatus>{};
    for (const auto& e : st.cache.registry->plugins) {
        const PluginStatus* s = nullptr;
        for (const auto& x : installed)
            if (x.folder.dir == e.id) s = &x;
        bool withdrawn = false;
        for (const auto& r : st.cache.registry->revoked) withdrawn = withdrawn || r.id == e.id;
        if (withdrawn && !s) continue;  // not offered any more
        Row row{e.id, oneLine(e.name), "", ""};
        const std::string author = oneLine(e.author), about = oneLine(e.description);
        row.byline = author.empty() ? about : about.empty() ? author : author + " · " + about;
        if (s) row.mark = s->folder.manifest && catalog_->updateFor(e.id, s->folder.manifest->version) ? "Update" : "Installed";
        all_.push_back(std::move(row));
    }
    std::stable_sort(all_.begin(), all_.end(), [](const Row& a, const Row& b) { return folded(a.name) < folded(b.name); });
    const int64_t offset = pager_.offset();
    pager_ = Pager(static_cast<int64_t>(all_.size()), perPage());
    while (pager_.offset() < offset && pager_.next()) {
    }
}

void CatalogListScreen::onShow(Nav& nav) {
    catalog_ = nav.catalog();
    if (catalog_) {
        catalog_->poll();
        catalog_->check(false);  // when the copy is more than an hour old
        wantCheck_ = !catalog_->state().checking && catalog_->busy();  // busy with something else: retried
    }
    load();
}

void CatalogListScreen::onTick(Nav& nav) {
    if (!catalog_) return;
    catalog_->poll();
    if (wantCheck_ && !catalog_->busy()) {
        wantCheck_ = false;
        catalog_->check(false);
    }
    if (catalog_->state().serial == serial_) return;
    load();
    nav.damage({0, 0, W(), H()}, Waveform::Ui);
}

std::string CatalogListScreen::headline() const {
    if (!catalog_) return "";
    const auto& st = catalog_->state();
    if (st.checking) return "Checking…";
    if (!st.cache.registry && st.noCatalog) return catalog::kNoPlugins;
    if (!st.cache.registry)
        return st.error.empty() || st.error == "No Wi-Fi." ? "Connect to Wi-Fi to see the plugins in the catalog." : st.error;
    std::string out = asOf(st.cache.registryMs);
    if (!st.error.empty() && st.doneOp == CatalogService::Op::Check) out += " · " + st.error;
    return out;
}

Rect CatalogListScreen::checkRect() const { return {W() - kMargin - 300, 30, 300, 80}; }

std::vector<CatalogListScreen::Row> CatalogListScreen::rows() const {
    const auto from = all_.begin() + std::min<int64_t>(pager_.offset(), all_.size());
    return {from, from + std::min<int64_t>(perPage(), all_.end() - from)};
}

std::vector<Rect> CatalogListScreen::rowRects() const {
    std::vector<Rect> out;
    const auto shown = rows();
    for (size_t i = 0; i < shown.size(); ++i)
        out.push_back({kMargin, kTop + kLineH + static_cast<int>(i) * kCatalogRowH, W() - 2 * kMargin, kCatalogRowH});
    return out;
}

void CatalogListScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    const Rect check = checkRect();
    drawHeader(cr, "Get plugins", true, check.w + 20);
    const bool checking = catalog_ && catalog_->state().checking;
    drawButton(cr, check, checking ? "Checking…" : "Check now", false);
    const TextStyle name{kSans, 9.5, 500}, quiet{kSans, 7, 400, false, kSecondary}, mark{kSans, 7.5, 500};
    const int w = W() - 2 * kMargin;
    const bool none = !catalog_ || !catalog_->state().cache.registry;
    drawText(cr, headline(), quiet, {kMargin, kTop + 14, w, none ? 120 : kLineH - 20}, none ? 2 : 1);
    if (none) return;
    hRule(cr, kTop + kLineH - 4, kMargin, W() - kMargin);
    if (all_.empty()) {  // a catalog with nothing in it yet
        drawText(cr, catalog::kNoPlugins, quiet, {kMargin, kTop + kLineH + 20, w, 60}, 1);
        return;
    }
    const auto shown = rows();
    const auto rects = rowRects();
    for (size_t i = 0; i < shown.size(); ++i) {
        const Rect r = rects[i];
        const int markW = shown[i].mark.empty() ? 0 : 240;
        drawText(cr, shown[i].name, name, {r.x, r.y + 26, r.w - markW - 20, 60}, 1);
        drawText(cr, shown[i].byline, quiet, {r.x, r.y + 92, r.w - markW - 20, 46}, 1);
        if (markW) centredText(cr, shown[i].mark, mark, {r.x + r.w - markW, r.y, markW, r.h}, Align::Right);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
    drawPageIndicator(cr, {0, H() - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void CatalogListScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (!catalog_) return;
    if (checkRect().contains(g.pos)) {
        catalog_->check(true);
        catalog_->poll();
        nav.damage({0, 0, W(), kTop + kLineH}, Waveform::Ui);
        return;
    }
    const auto shown = rows();
    const auto rects = rowRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(g.pos)) continue;
        if (shown[i].mark.empty()) nav.push(std::make_unique<CatalogPageScreen>(shown[i].id, host_));
        else nav.push(std::make_unique<PluginPageScreen>(shown[i].id, host_));
        return;
    }
}

}  // namespace zk
