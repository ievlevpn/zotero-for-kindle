#include "screens/catalog_page.h"

#include <algorithm>

#include "app/catalog_service.h"
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
constexpr int kTop = kHeaderH + 10;
constexpr int kButtonH = 92;

const TextStyle kByline{kSans, 7.5, 400, false, kSecondary};
const TextStyle kAbout{kSerif, 8, 400};
const TextStyle kQuiet{kSans, 7.5, 400, false, kSecondary};
const TextStyle kAlarm{kSans, 8, 500};

int text(cairo_t* cr, const std::string& s, const TextStyle& st, Rect box, int maxLines) {
    if (maxLines <= 0 || box.h <= 0) return 0;
    return cr ? drawText(cr, s, st, box, maxLines).height : measureText(s, st, box.w, maxLines).height;
}

const catalog::Entry* entryOf(CatalogService* c, const std::string& id) { return c ? catalog::find(c->state().cache, id) : nullptr; }

}  // namespace

CatalogPageScreen::CatalogPageScreen(std::string id, PluginHostFn host) : id_(std::move(id)), host_(std::move(host)) {}

void CatalogPageScreen::onShow(Nav& nav) {
    catalog_ = nav.catalog();
    if (!catalog_) return;
    catalog_->poll();
    catalog_->lookUp(id_);  // its version, unless known from the last 6 h
    lookedUp_ = !catalog_->busy() || catalog_->state().busy == CatalogService::Op::LookUp;
    serial_ = catalog_->state().serial;
}

void CatalogPageScreen::onTick(Nav& nav) {
    if (!catalog_) return;
    catalog_->poll();
    if (!lookedUp_ && !catalog_->busy()) {  // the catalog was busy when the page opened
        lookedUp_ = true;
        catalog_->lookUp(id_);
    }
    const auto& st = catalog_->state();
    if (st.serial == serial_) return;
    serial_ = st.serial;
    if (asked_ && st.installed.count(id_)) {  // installed: part 1's page shows what it can do and how to turn it on
        PluginHostFn host = host_;
        const std::string id = id_;
        nav.pop();
        nav.push(std::make_unique<PluginPageScreen>(id, host));
        return;
    }
    nav.damage({0, 0, W(), H()}, Waveform::Ui);
}

std::string CatalogPageScreen::byline() const {
    const catalog::Entry* e = entryOf(catalog_, id_);
    if (!e) return "";
    std::string out;
    if (auto l = catalog_->state().cache.latest.find(id_); l != catalog_->state().cache.latest.end())
        out = "Version " + l->second.version.str();
    if (!oneLine(e->author).empty()) out += (out.empty() ? "" : " · ") + oneLine(e->author);
    return out;
}

std::string CatalogPageScreen::installLabel() const {
    if (catalog_ && ((catalog_->state().busy == CatalogService::Op::Install && catalog_->state().busyId == id_) ||
                     catalog_->state().queued.count(id_)))
        return "Downloading…";
    return "Install";
}

std::string CatalogPageScreen::failure() const {
    if (!catalog_) return "";
    const auto& st = catalog_->state();
    if (st.queued.count(id_) || (st.busy == CatalogService::Op::Install && st.busyId == id_)) return "";
    auto f = st.installFailed.find(id_);
    return f == st.installFailed.end() ? "" : f->second;
}

CatalogPageScreen::Layout CatalogPageScreen::place(cairo_t* cr) const {
    Layout l;
    const int x = kMargin, w = W() - 2 * kMargin;
    int y = kTop;
    const catalog::Entry* e = entryOf(catalog_, id_);
    if (!e) {
        text(cr, "This plugin isn't in the catalog any more.", kQuiet, {x, y + 10, w, 120}, 2);
        return l;
    }
    if (const std::string by = byline(); !by.empty()) y += text(cr, by, kByline, {x, y, w, 50}, 1) + 24;
    if (!oneLine(e->description).empty()) y += text(cr, e->description, kAbout, {x, y, w, 60 * 6}, 6) + 24;
    y += text(cr, "github.com/" + e->repo, kQuiet, {x, y, w, 50}, 1) + 40;
    y += text(cr, "What it can do shows once it is downloaded. It stays off until you turn it on.", kQuiet, {x, y, w, 120}, 2) + 30;
    l.install = {x, std::min(y, H() - 200 - kButtonH), 360, kButtonH};
    if (cr) drawButton(cr, l.install, installLabel(), true);
    y = l.install.y + kButtonH + 30;
    if (const std::string why = failure(); !why.empty()) text(cr, why, kAlarm, {x, y, w, 120}, 2);
    return l;
}

Rect CatalogPageScreen::installRect() const { return place(nullptr).install; }

void CatalogPageScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    const catalog::Entry* e = entryOf(catalog_, id_);
    drawHeader(cr, e ? oneLine(e->name) : "Plugin", true);
    place(cr);
}

void CatalogPageScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    const Rect r = installRect();
    if (!catalog_ || r.empty() || !r.contains(g.pos) || installLabel() != "Install") return;
    asked_ = true;
    catalog_->install(id_);
    catalog_->poll();
    serial_ = catalog_->state().serial;
    nav.damage({0, 0, W(), H()}, Waveform::Ui);
}

}  // namespace zk
