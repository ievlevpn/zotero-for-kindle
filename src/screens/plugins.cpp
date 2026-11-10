#include "screens/plugins.h"

#include <algorithm>

#include <glib.h>

#include "app/catalog_service.h"
#include "screens/catalog_list.h"
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
constexpr int kListRowH = 160;
constexpr int kTop = kHeaderH + 10;
constexpr int kOffLineH = 120;  // "Plugins are off: …" above the list
constexpr int kUpdateAllH = 120;  // "Update all (n)" above the list

std::string folded(const std::string& s) {
    gchar* f = g_utf8_casefold(s.c_str(), -1);
    std::string out = f ? f : s;
    g_free(f);
    return out;
}

bool canTurnOn(const PluginStatus& s) { return s.folder.manifest && s.folder.problem.empty() && s.withdrawn.empty(); }

}  // namespace

std::string pluginName(const PluginStatus& s) {
    if (s.folder.manifest) {
        std::string n = oneLine(s.folder.manifest->name);
        if (!n.empty()) return n;
    }
    return oneLine(s.folder.dir);
}

std::string pluginNote(const PluginStatus& s) {
    if (!s.folder.problem.empty()) {
        if (s.folder.incompatible && s.folder.otherApi != 0)
            return "Incompatible: written for plugin API " + std::to_string(s.folder.otherApi);
        if (s.folder.incompatible && s.folder.manifest) return "Incompatible: needs app " + s.folder.manifest->minApp.str();
        return "Can't load: " + oneLine(s.folder.problem);
    }
    if (!s.withdrawn.empty()) return "Turned off — withdrawn from the catalog: " + oneLine(s.withdrawn);
    if (s.stopped) return "Turned off — it stopped working";
    if (!s.problem.empty()) return "Can't load: " + oneLine(s.problem);
    if (!s.enabled && !s.everEnabled) return "New — not turned on yet";
    if (s.enabled && s.failures > 0 && !s.errors.empty()) return oneLine(s.errors.back());
    if (s.enabled && !s.badge.empty()) return oneLine(s.badge);
    return s.folder.manifest ? oneLine(s.folder.manifest->description) : "";
}

std::string pluginNote(const PluginStatus& s, CatalogService* catalog) {
    if (catalog && s.folder.manifest && !pluginNeedsAttention(s) && s.withdrawn.empty())
        if (auto l = catalog->updateFor(s.folder.dir, s.folder.manifest->version)) return "Update to " + l->version.str();
    return pluginNote(s);
}

bool pluginNeedsAttention(const PluginStatus& s) {
    return !s.folder.problem.empty() || !s.withdrawn.empty() || s.stopped || !s.problem.empty() || (s.enabled && s.failures > 0);
}

std::string pluginsSummary(PluginHost* host) {
    if (!host) return "Not available";
    const auto all = host->status();
    if (all.empty()) return "None installed";
    int on = 0, attention = 0;
    for (const auto& s : all) {
        if (s.enabled && canTurnOn(s)) ++on;
        if (pluginNeedsAttention(s)) ++attention;
    }
    std::string out = std::to_string(on) + " on";
    if (attention > 0) out += " · " + std::to_string(attention) + (attention == 1 ? " needs attention" : " need attention");
    return out;
}

PluginsScreen::PluginsScreen(PluginHostFn host, PluginsOff off) : host_(std::move(host)), off_(std::move(off)) {}

int PluginsScreen::waiting() const { return catalog_ ? catalog_->updatesForAll() : 0; }

int PluginsScreen::top() const { return kTop + (off_.reason.empty() ? 0 : kOffLineH) + (waiting() > 0 ? kUpdateAllH : 0); }

Rect PluginsScreen::updateAllRect() const {
    if (waiting() <= 0) return {};
    return {kMargin, kTop + (off_.reason.empty() ? 0 : kOffLineH) + 16, 480, 84};
}

std::string PluginsScreen::updateAllLabel() const {
    if (catalog_ && catalog_->state().busy == CatalogService::Op::Update && catalog_->state().busyId.empty())
        return catalog_->state().busyText;
    return "Update all (" + std::to_string(waiting()) + ")";
}

Rect PluginsScreen::getPluginsRect() const {
    if (!catalog_ || !host_ || !host_()) return {};
    if (plugins_.empty()) return {kMargin, top() + 200, W() - 2 * kMargin, kListRowH};
    const int64_t row = static_cast<int64_t>(plugins_.size()) - pager_.offset();
    if (row < 0 || row >= perPage()) return {};
    return {kMargin, top() + static_cast<int>(row) * kListRowH, W() - 2 * kMargin, kListRowH};
}

int PluginsScreen::perPage() const { return std::max(1, (H() - top() - kPageIndicatorH) / kListRowH); }

void PluginsScreen::load() {
    PluginHost* h = host_ ? host_() : nullptr;
    plugins_ = h ? h->status() : std::vector<PluginStatus>{};
    std::stable_sort(plugins_.begin(), plugins_.end(), [](const PluginStatus& a, const PluginStatus& b) {
        return folded(pluginName(a)) < folded(pluginName(b));
    });
    const int64_t offset = pager_.offset();
    pager_ = Pager(static_cast<int64_t>(plugins_.size()) + (catalog_ && !plugins_.empty() ? 1 : 0), perPage());
    while (pager_.offset() < offset && pager_.next()) {
    }
    if (catalog_) catalogSerial_ = catalog_->state().serial;
    hostVersion_ = h ? h->entriesVersion() : 0;
}

void PluginsScreen::onShow(Nav& nav) {
    catalog_ = nav.catalog();
    load();
    if (off_.seen) off_.seen();
}

void PluginsScreen::onTick(Nav& nav) {
    PluginHost* h = host_ ? host_() : nullptr;
    if (catalog_) catalog_->poll();
    const bool changed = (catalog_ && catalog_->state().serial != catalogSerial_) || (h && h->entriesVersion() != hostVersion_);
    if (!changed) return;
    if (catalog_ && catalog_->state().serial != catalogSerial_) {  // what Update all couldn't do, said
        const auto& st = catalog_->state();
        if (st.busy == CatalogService::Op::None && st.doneOp == CatalogService::Op::Update && st.doneId.empty() &&
            !st.error.empty())
            nav.toast(st.error);
    }
    load();
    nav.damage({0, 0, W(), H()}, Waveform::Ui);
}

const PluginStatus* PluginsScreen::at(int row) const {
    const int64_t i = pager_.offset() + row;
    if (row < 0 || i >= static_cast<int64_t>(plugins_.size())) return nullptr;
    return &plugins_[static_cast<size_t>(i)];
}

std::vector<Rect> PluginsScreen::rowRects() const {
    std::vector<Rect> out;
    for (int i = 0; i < perPage() && at(i); ++i) out.push_back({kMargin, top() + i * kListRowH, W() - 2 * kMargin, kListRowH});
    return out;
}

Rect PluginsScreen::toggleRect(int row) const {
    const PluginStatus* s = at(row);
    if (!s || !canTurnOn(*s)) return {};
    const Rect r{kMargin, top() + row * kListRowH, W() - 2 * kMargin, kListRowH};
    return {r.x + r.w - 140, r.y + (r.h - 80) / 2, 140, 80};
}

std::string PluginsScreen::note(int row) const {
    const PluginStatus* s = at(row);
    return s ? pluginNote(*s, catalog_) : "";
}

Rect PluginsScreen::turnAllOffRect() const {
    for (const auto& s : plugins_)
        if (s.enabled) return {W() - kMargin - 440, 30, 440, 80};
    return {};
}

void PluginsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    const Rect all = turnAllOffRect();
    drawHeader(cr, "Plugins", true, all.empty() ? 0 : all.w + 20);
    if (!all.empty()) drawButton(cr, all, "Turn all plugins off", false);
    const TextStyle name{kSans, 9.5, 500}, version{kSans, 7, 400, false, kTertiary}, quiet{kSans, 7, 400, false, kSecondary},
        loud{kSans, 7, 500};
    const int w = W() - 2 * kMargin;
    if (!off_.reason.empty()) {
        drawText(cr, off_.reason, loud, {kMargin, kTop + 14, w, kOffLineH - 20}, 2);
        hRule(cr, kTop + kOffLineH - 4, kMargin, W() - kMargin);
    }
    if (const Rect u = updateAllRect(); !u.empty()) {
        drawButton(cr, u, updateAllLabel(), true);
        hRule(cr, top() - 4, kMargin, W() - kMargin);
    }
    if (const Rect g = getPluginsRect(); !g.empty()) {
        centredText(cr, "Get plugins", name, {g.x, g.y, g.w - 60, g.h}, Align::Left);
        centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {g.x + g.w - 40, g.y, 40, g.h}, Align::Right);
        hRule(cr, g.y + g.h - 2, g.x, g.x + g.w);
    }
    if (!host_ || !host_()) {
        drawText(cr, "Plugins aren't available right now.", quiet, {kMargin, top() + 20, w, 120}, 2);
        return;
    }
    if (plugins_.empty()) {
        drawText(cr, "No plugins installed. To add one, connect the Kindle by USB and copy the plugin's folder into "
                     "zotero/plugins.",
                 quiet, {kMargin, top() + 20, w, 160}, 3);
        return;
    }
    const auto rows = rowRects();
    for (size_t i = 0; i < rows.size(); ++i) {
        const Rect r = rows[i];
        const PluginStatus& s = *at(static_cast<int>(i));
        const Rect toggle = toggleRect(static_cast<int>(i));
        const int textW = r.w - 180;
        const std::string n = pluginName(s);
        const int nameW = drawText(cr, n, name, {r.x, r.y + 26, textW, 60}, 1).width;
        if (s.folder.manifest && nameW + 30 < textW)
            drawText(cr, oneLine(s.folder.manifest->version), version, {r.x + nameW + 20, r.y + 40, textW - nameW - 20, 44}, 1);
        drawText(cr, pluginNote(s, catalog_), pluginNeedsAttention(s) ? loud : quiet, {r.x, r.y + 92, textW, 46}, 1);
        if (!toggle.empty()) drawToggle(cr, toggle, s.enabled);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
    drawPageIndicator(cr, {0, H() - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void PluginsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    PluginHost* host = host_ ? host_() : nullptr;
    if (!host) return;
    if (const Rect u = updateAllRect(); !u.empty() && u.contains(g.pos)) {
        catalog_->updateAll();
        catalog_->poll();
        nav.damage(u, Waveform::Ui);
        return;
    }
    if (const Rect gp = getPluginsRect(); !gp.empty() && gp.contains(g.pos)) {
        nav.push(std::make_unique<CatalogListScreen>(host_));
        return;
    }
    if (const Rect all = turnAllOffRect(); !all.empty() && all.contains(g.pos)) {
        for (const auto& s : plugins_)
            if (s.enabled) host->setEnabled(s.folder.dir, false);
        load();
        nav.damage({0, 0, W(), H()}, Waveform::Ui);
        return;
    }
    const auto rows = rowRects();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].contains(g.pos)) continue;
        const PluginStatus& s = *at(static_cast<int>(i));
        // The toggle, and a little around it: an e-ink finger is not precise.
        const Rect t = toggleRect(static_cast<int>(i));
        const bool onToggle = !t.empty() && g.pos.x >= t.x - 40;
        if (onToggle && (s.enabled || s.everEnabled)) {
            const bool hadAll = !turnAllOffRect().empty();
            host->setEnabled(s.folder.dir, !s.enabled);  // `s` goes with the reload below
            load();
            nav.damage(rows[i], Waveform::Ui);
            if (hadAll != !turnAllOffRect().empty()) nav.damage({0, 0, W(), kHeaderH}, Waveform::Ui);  // the top action
            return;
        }
        // Never turned on (its permissions come first), can't be turned on, or the row itself.
        nav.push(std::make_unique<PluginPageScreen>(s.folder.dir, host_));
        return;
    }
}

}  // namespace zk
