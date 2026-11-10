#include "screens/plugin_page.h"

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>

#include "app/catalog_service.h"
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
constexpr int kTurnH = 92;
constexpr int kLabelH = 58;  // "What it can do" and the space under it
constexpr int kMoreH = 60;   // "+N more ›"
// A repair at the bottom: one line of text between rules (a little lower on a short window).
int actionH() { return H() < 1500 ? 88 : 100; }

const TextStyle kByline{kSans, 7.5, 400, false, kSecondary};
const TextStyle kAbout{kSerif, 8, 400};
const TextStyle kLabel{kSans, 7, 500, false, kSecondary};
const TextStyle kLine{kSans, 8, 400};
const TextStyle kAlarm{kSans, 8, 500};
const TextStyle kQuiet{kSans, 7.5, 400, false, kSecondary};
const TextStyle kAction{kSans, 9, 500};

std::string count(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// One section's text: drawn when `cr` is given, else only measured; its height.
int text(cairo_t* cr, const std::string& s, const TextStyle& st, Rect box, int maxLines) {
    if (maxLines <= 0 || box.h <= 0) return 0;
    return cr ? drawText(cr, s, st, box, maxLines).height : measureText(s, st, box.w, maxLines).height;
}

}  // namespace

// ---- The page ------------------------------------------------------------------------------

PluginPageScreen::PluginPageScreen(std::string id, PluginHostFn host) : id_(std::move(id)), host_(std::move(host)) {}

void PluginPageScreen::load(Nav& nav) {
    status_.reset();
    hostVersion_ = 0;
    if (PluginHost* h = host_ ? host_() : nullptr) {
        hostVersion_ = h->entriesVersion();
        for (auto& s : h->status())
            if (s.folder.dir == id_) status_ = std::move(s);
    }
    if (catalog_) catalogSerial_ = catalog_->state().serial;
    pending_ = static_cast<int>(nav.library().pendingPluginChanges(id_).size());
}

void PluginPageScreen::onShow(Nav& nav) {
    catalog_ = nav.catalog();
    if (catalog_) catalog_->poll();
    load(nav);
    ask_ = Ask::None;
    wantCheck_ = true;
    checkForUpdate();
}

// An installed catalog plugin's page checks the catalog as Get plugins does (when the copy is more
// than an hour old), and its own release when that is over an hour old, so its Update shows here;
// while the catalog is busy, it waits its turn.
void PluginPageScreen::checkForUpdate() {
    if (!wantCheck_ || !catalog_ || catalog_->busy()) return;
    wantCheck_ = false;
    const auto& cache = catalog_->state().cache;
    if (!status_ || (!catalog::find(cache, id_) && !cache.fromCatalog.count(id_))) return;
    catalog_->check(false, id_);  // its own release too, when over an hour old
}

void PluginPageScreen::onTick(Nav& nav) {
    PluginHost* h = host_ ? host_() : nullptr;
    if (catalog_) catalog_->poll();
    checkForUpdate();
    const int serial = catalog_ ? catalog_->state().serial : -1;
    if (serial == catalogSerial_ && (h ? h->entriesVersion() : 0) == hostVersion_) return;
    if (catalog_ && serial != catalogSerial_ && !removing_.empty()) {  // the Remove the user confirmed
        const auto& st = catalog_->state();
        if (st.removed.count(id_)) {
            nav.toast(removing_ + " was removed.");
            removing_.clear();
            nav.pop();
            return;
        }
        if (auto f = st.removeFailed.find(id_); f != st.removeFailed.end()) {
            nav.toast(f->second);
            removing_.clear();
        }
    }
    if (catalog_ && serial != catalogSerial_) {
        const auto& st = catalog_->state();
        using Op = CatalogService::Op;
        const bool ours = st.doneId == id_ && (st.doneOp == Op::Update || st.doneOp == Op::Apply || st.doneOp == Op::RollBack);
        if (st.busy == Op::None && ours && !st.doneOk && !st.error.empty()) nav.toast(st.error);
        if (st.busy == Op::None && ours && st.doneOk && !st.notice.empty()) nav.toast(st.notice);
    }
    load(nav);
    nav.damage({0, 0, W(), H()}, Waveform::Ui);
}

std::string PluginPageScreen::provenance() const {
    if (!catalog_ || !status_) return "";
    return catalog_->state().cache.fromCatalog.count(id_) ? "From the catalog" : "Installed by hand";
}

std::string PluginPageScreen::updateLabel() const {
    if (!catalog_ || !status_ || !status_->folder.manifest || !status_->withdrawn.empty()) return "";
    const auto& st = catalog_->state();
    using Op = CatalogService::Op;
    if (st.busyId == id_ && (st.busy == Op::Update || st.busy == Op::Apply)) return st.busyText;
    if (auto q = st.queued.find(id_); q != st.queued.end() && q->second == "Downloading…") return q->second;
    if (reasking()) return "";
    auto l = catalog_->updateFor(id_, status_->folder.manifest->version);
    return l ? "Update to " + l->version.str() : "";
}

std::string PluginPageScreen::rollBackLabel() const {
    if (!catalog_ || !status_) return "";
    const auto& kept = catalog_->state().kept;
    auto it = kept.find(id_);
    if (it == kept.end()) return "";
    const auto& st = catalog_->state();
    if (st.busyId == id_ && st.busy == CatalogService::Op::RollBack) return st.busyText;
    if (auto q = st.queued.find(id_); q != st.queued.end() && q->second == "Rolling back…") return q->second;
    return "Roll back to " + oneLine(it->second);
}

bool PluginPageScreen::reasking() const {
    return catalog_ && catalog_->state().awaitingYes && catalog_->state().awaitingYes->manifest.id == id_ &&
           catalog_->state().busy == CatalogService::Op::None;
}

std::string PluginPageScreen::name() const { return status_ ? pluginName(*status_) : "Plugin"; }

std::vector<std::string> permissionLinesOf(const PluginManifest& m, const std::vector<Permission>* only, const PluginManifest* old) {
    std::vector<std::string> out;
    for (Permission p : m.permissions) {
        if (only && std::find(only->begin(), only->end(), p) == only->end()) continue;
        if (p != Permission::Net) {
            out.push_back(permissionText(p));
            continue;
        }
        for (const auto& host : m.hosts) {
            if (old && std::find(old->hosts.begin(), old->hosts.end(), host) != old->hosts.end()) continue;
            out.push_back(host.rfind("*.", 0) == 0 ? "Connects to sites under " + host.substr(2) : permissionText(p, host));
        }
    }
    return out;
}

std::vector<std::string> PluginPageScreen::permissionLines() const {
    if (!status_ || !status_->folder.manifest) return {};
    return permissionLinesOf(*status_->folder.manifest);
}

std::vector<std::string> PluginPageScreen::errorLines() const {
    std::vector<std::string> out;
    if (!status_) return out;
    for (auto it = status_->errors.rbegin(); it != status_->errors.rend() && out.size() < 3; ++it) out.push_back(oneLine(*it));
    return out;
}

std::string PluginPageScreen::turnLabel() const {
    if (!status_ || !status_->folder.manifest || !status_->folder.problem.empty() || !status_->withdrawn.empty()) return "";
    return status_->enabled ? "Turn off" : "Turn on";
}

std::string PluginPageScreen::discardLabel() const { return "Discard " + count(pending_, "unsent change", "unsent changes"); }

// Top to bottom: who made it, what it is and its badge, a problem, What it can do, Turn on/off,
// its last errors; the repairs sit at the bottom. Measured without `cr`, drawn with it.
PluginPageScreen::Layout PluginPageScreen::place(cairo_t* cr) const {
    Layout l;
    const int x = kMargin, w = W() - 2 * kMargin;
    // The repairs, from the bottom up.
    std::vector<Rect*> actions;
    const bool manifest = status_ && status_->folder.manifest;
    if (manifest) actions.push_back(&l.changes);
    if (manifest) actions.push_back(&l.reset);
    if (status_ && pending_ > 0) actions.push_back(&l.discard);
    const std::string rollBack = rollBackLabel();
    if (!rollBack.empty()) actions.push_back(&l.rollBack);
    if (status_) actions.push_back(&l.remove);
    const int actionsTop = H() - 30 - static_cast<int>(actions.size()) * actionH();
    for (size_t i = 0; i < actions.size(); ++i) *actions[i] = {x, actionsTop + static_cast<int>(i) * actionH(), w, actionH()};
    const int bottom = actionsTop - 30;  // what's above stops here

    int y = kTop;
    auto room = [&](int lineH) { return std::max(0, (bottom - y) / lineH); };
    if (!status_) {
        text(cr, host_ && host_() ? "This plugin isn't installed any more." : "Plugins aren't available right now.", kQuiet,
             {x, y + 10, w, 120}, 2);
        return l;
    }
    const PluginStatus& s = *status_;
    const bool canTurn = !turnLabel().empty();
    const std::string update = updateLabel();
    const int turnH = canTurn || !update.empty() ? kTurnH + 30 : 0;
    auto lines = permissionLines();
    if (manifest && lines.empty()) lines.push_back("Nothing that needs your permission");
    auto lineH = [&](const std::string& line) { return measureText(line, kLine, w, 2).height + 8; };
    // What must stay on the page below the description: the label, the first permission (and
    // "+N more" when there are others), Turn on/off.
    const int permsMin = manifest ? kLabelH + lineH(lines[0]) + (lines.size() > 1 ? kMoreH : 0) + 20 : 0;
    const bool badge = s.enabled && !oneLine(s.badge).empty();
    const bool alarm = !s.folder.problem.empty() || s.stopped || !s.problem.empty();
    const int badgeH = badge ? measureText(oneLine(s.badge), kQuiet, w, 1).height + 24 : 0;
    const int alarmH = alarm ? measureText(pluginNote(s), kAlarm, w, 2).height + 30 : 0;

    if (manifest) {
        const PluginManifest& m = *s.folder.manifest;
        std::string by = "Version " + oneLine(m.version);
        if (!oneLine(m.author).empty()) by += " · " + oneLine(m.author);
        y += text(cr, by, kByline, {x, y, w, 50}, 1) + 24;
        if (const std::string from = provenance(); !from.empty()) y += text(cr, from, kQuiet, {x, y - 12, w, 50}, 1) + 12;
        if (!oneLine(m.description).empty()) {
            // Up to 3 lines (then "…"), fewer when what follows needs the room.
            int n = 3;
            while (n > 1 && y + measureText(m.description, kAbout, w, n).height + 24 + badgeH + alarmH + permsMin + turnH > bottom) --n;
            y += text(cr, m.description, kAbout, {x, y, w, 60 * n}, n) + 24;
        }
        if (badge) y += text(cr, oneLine(s.badge), kQuiet, {x, y, w, 50}, 1) + 24;  // what it says of itself now
        y += 6;
    }
    if (alarm) y += text(cr, pluginNote(s), kAlarm, {x, y, w, 120}, 2) + 30;

    if (manifest) {
        y += text(cr, "What it can do", kLabel, {x, y, w, 44}, 1) + (kLabelH - 44);
        // As many as fit above Turn on/off; the rest behind "+N more" (a page of its own).
        const int room = bottom - turnH - 20 - y;
        int used = 0;
        size_t shown = 0;
        for (const auto& line : lines) {
            const bool last = shown + 1 == lines.size();
            if (used + lineH(line) + (last ? 0 : kMoreH) > room && lines.size() > 1) break;  // a lone one always shows
            used += lineH(line);
            ++shown;
        }
        for (size_t i = 0; i < shown; ++i) y += text(cr, lines[i], kLine, {x, y, w, 120}, 2) + 8;
        l.permissions = static_cast<int>(shown);
        if (shown < lines.size()) {
            l.more = {x, y, w, kMoreH};
            if (cr) {
                centredText(cr, "+" + std::to_string(lines.size() - shown) + " more", {kSans, 8, 500}, {x, y, w - 60, kMoreH},
                            Align::Left);
                centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {x + w - 40, y, 40, kMoreH}, Align::Right);
            }
            y += kMoreH;
        }
        y += 20;
    }
    if (canTurn || !update.empty()) {  // always on the page: above the repairs at worst
        const Rect row{x, std::min(y, bottom - kTurnH), 360, kTurnH};
        if (cr) fillRect(cr, {x, row.y - 10, w, kTurnH + 20}, 255);
        if (canTurn) {
            l.turn = row;
            if (cr) drawButton(cr, l.turn, turnLabel(), !s.enabled);
        }
        if (!update.empty()) {
            l.update = canTurn ? Rect{x + 400, row.y, std::min(460, w - 400), kTurnH} : Rect{x, row.y, 460, kTurnH};
            if (cr) drawButton(cr, l.update, update, false);
        }
        y = row.y + kTurnH + 40;
    }
    const auto errors = errorLines();
    if (!errors.empty() && y + 44 + 10 + 44 <= bottom) {
        y += text(cr, errors.size() == 1 ? "Last error" : "Last errors", kLabel, {x, y, w, 44}, 1) + 10;
        for (const auto& e : errors) {
            if (room(44) == 0) break;
            y += text(cr, e, kQuiet, {x, y, w, bottom - y}, std::min(2, room(44))) + 12;
        }
    }

    if (cr) {
        hRule(cr, actionsTop, x, x + w);
        auto row = [&](const Rect& r, const std::string& label, bool more) {
            if (r.empty()) return;
            centredText(cr, label, kAction, {r.x, r.y, r.w - 60, r.h}, Align::Left);
            if (more) centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {r.x + r.w - 40, r.y, 40, r.h}, Align::Right);
            hRule(cr, r.y + r.h, r.x, r.x + r.w);
        };
        row(l.changes, "What it changed", true);
        row(l.reset, "Reset its data", false);
        row(l.discard, discardLabel(), false);
        row(l.rollBack, rollBack, false);
        row(l.remove, removing_.empty() ? "Remove" : "Removing…", false);
    }
    return l;
}

Rect PluginPageScreen::turnRect() const { return layout().turn; }
Rect PluginPageScreen::moreRect() const { return layout().more; }
int PluginPageScreen::shownPermissions() const { return layout().permissions; }
Rect PluginPageScreen::changesRect() const { return layout().changes; }
Rect PluginPageScreen::resetRect() const { return layout().reset; }
Rect PluginPageScreen::discardRect() const { return layout().discard; }
Rect PluginPageScreen::removeRect() const { return layout().remove; }
Rect PluginPageScreen::updateRect() const { return layout().update; }
Rect PluginPageScreen::rollBackRect() const { return layout().rollBack; }

std::string PluginPageScreen::question() const {
    const std::string n = name();
    switch (ask_) {
        case Ask::None: return "";
        case Ask::Reset: return "Reset " + n + "’s data? What it stored on the Kindle is deleted; its changes in Zotero stay.";
        case Ask::Discard:
            return "Discard " + count(pending_, "change ", "changes ") + n + " hasn't sent to Zotero yet? " +
                   (pending_ == 1 ? "It won't reach Zotero." : "They won't reach Zotero.");
        case Ask::Remove:
            if (catalog_)
                return "Remove " + n + " and the data it saved?" +
                       (pending_ > 0 ? " " + count(pending_, "change", "changes") + " not synced yet will still be sent." : "");
            return "Remove " + n + "? Its folder is deleted." +
                   (pending_ > 0 ? " " + count(pending_, "change", "changes") + " not synced yet will still be sent." : "");
    }
    return "";
}

std::optional<PluginConfirm> PluginPageScreen::confirm() const {
    if (ask_ == Ask::None && reasking()) {
        const auto& staged = *catalog_->state().awaitingYes;
        std::vector<std::string> lines{"This version also wants to:"};
        const PluginManifest* old = status_ && status_->folder.manifest ? &*status_->folder.manifest : nullptr;
        for (auto& line : permissionLinesOf(staged.manifest, &staged.added, old)) lines.push_back(std::move(line));
        return PluginConfirm(name(), lines, "Update", "Not now");
    }
    if (ask_ == Ask::None) return std::nullopt;
    const char* action = ask_ == Ask::Reset ? "Reset" : ask_ == Ask::Discard ? "Discard" : "Remove";
    return PluginConfirm(name(), {question()}, action);
}

std::vector<std::string> PluginPageScreen::confirmLines() const {
    const auto c = confirm();
    return c ? c->lines() : std::vector<std::string>{};
}

Rect PluginPageScreen::yesRect() const {
    const auto c = confirm();
    return c ? c->yesRect() : Rect{};
}

Rect PluginPageScreen::noRect() const {
    const auto c = confirm();
    return c ? c->noRect() : Rect{};
}

void PluginPageScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, name(), true);
    place(cr);
    if (const auto c = confirm()) c->draw(cr);
}

void PluginPageScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    PluginHost* host = host_ ? host_() : nullptr;
    const Rect whole{0, 0, W(), H()};
    if (const auto c = confirm()) {
        const std::optional<bool> yes = c->onTap(g.pos);
        if (!yes && c->rect().contains(g.pos)) return;  // on the box, not a button
        const Ask asked = ask_;
        ask_ = Ask::None;
        if (asked == Ask::None) {  // the re-ask: the installed version runs until the yes; only its buttons answer
            if (!yes) return;
            if (*yes) catalog_->applyStaged(id_);
            else catalog_->discardStaged(id_);
            catalog_->poll();
            load(nav);
            nav.damage(whole, Waveform::Ui);
            return;
        }
        if (yes.value_or(false) && asked == Ask::Remove && catalog_ && status_) {  // files, storage and state, on its worker
            removing_ = name();  // said once it has worked (onTick); queued if the catalog is busy
            catalog_->remove(id_);
            load(nav);
            nav.damage(whole, Waveform::Ui);
            return;
        }
        if (yes.value_or(false) && host && status_) {
            if (asked == Ask::Reset) {
                host->resetData(id_);
                nav.toast("Its data was reset.");
            } else if (asked == Ask::Discard) {
                const int n = nav.library().discardPluginChanges(id_);
                nav.toast(count(n, "change", "changes") + " discarded.");
            } else if (asked == Ask::Remove) {
                const std::string n = name();
                host->remove(id_);
                nav.toast(n + " was removed.");
                nav.pop();
                return;
            }
        }
        load(nav);
        nav.damage(whole, Waveform::Ui);
        return;
    }
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (!host || !status_) return;
    const Layout l = layout();
    if (!l.turn.empty() && l.turn.contains(g.pos)) {
        host->setEnabled(id_, !status_->enabled);
        load(nav);
        nav.damage(whole, Waveform::Ui);
        return;
    }
    if (!l.update.empty() && l.update.contains(g.pos)) {
        catalog_->update(id_);  // waits its turn behind a check
        catalog_->poll();
        nav.damage(whole, Waveform::Ui);
        return;
    }
    if (!l.rollBack.empty() && l.rollBack.contains(g.pos)) {
        catalog_->rollBack(id_);
        catalog_->poll();
        nav.damage(whole, Waveform::Ui);
        return;
    }
    if (!l.more.empty() && l.more.contains(g.pos)) {
        nav.push(std::make_unique<PluginPermissionsScreen>(name(), permissionLines()));
        return;
    }
    if (!l.changes.empty() && l.changes.contains(g.pos)) {
        nav.push(std::make_unique<PluginChangesScreen>(id_, name()));
        return;
    }
    const Ask ask = !l.reset.empty() && l.reset.contains(g.pos)       ? Ask::Reset
                    : !l.discard.empty() && l.discard.contains(g.pos) ? Ask::Discard
                    : !l.remove.empty() && l.remove.contains(g.pos) && removing_.empty() ? Ask::Remove
                                                                       : Ask::None;
    if (ask == Ask::None) return;
    load(nav);  // the count it states is the count now
    ask_ = ask;
    nav.damage(whole, Waveform::Ui);
}

// ---- What it can do (all of it) ------------------------------------------------------------

namespace {
constexpr int kPermissionH = 120;  // two lines at most
}

PluginPermissionsScreen::PluginPermissionsScreen(std::string pluginName, std::vector<std::string> lines)
    : name_(std::move(pluginName)), all_(std::move(lines)) {
    pager_ = Pager(static_cast<int64_t>(all_.size()), perPage());
}

int PluginPermissionsScreen::perPage() const { return std::max(1, (H() - kTop - kPageIndicatorH) / kPermissionH); }

void PluginPermissionsScreen::onShow(Nav&) {
    const int64_t offset = pager_.offset();
    pager_ = Pager(static_cast<int64_t>(all_.size()), perPage());
    while (pager_.offset() < offset && pager_.next()) {
    }
}

std::vector<std::string> PluginPermissionsScreen::lines() const {
    const auto from = all_.begin() + std::min<int64_t>(pager_.offset(), all_.size());
    return {from, from + std::min<int64_t>(perPage(), all_.end() - from)};
}

void PluginPermissionsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "What " + name_ + " can do", true);
    const auto shown = lines();
    for (size_t i = 0; i < shown.size(); ++i) {
        const Rect r{kMargin, kTop + static_cast<int>(i) * kPermissionH, W() - 2 * kMargin, kPermissionH};
        const int h = measureText(shown[i], kLine, r.w, 2).height;
        drawText(cr, shown[i], kLine, {r.x, r.y + (r.h - h) / 2, r.w, h + 4}, 2);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
    drawPageIndicator(cr, {0, H() - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void PluginPermissionsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
        return;
    }
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos)) nav.pop();
}

// ---- What it changed -----------------------------------------------------------------------

namespace {

// A change: what and the outcome, the item; a refused one has its reason under it, in more room.
constexpr int kChangeH = 150, kRefusedH = 200;
int heightOf(const PluginChangesScreen::Line& l) { return l.outcome.find(": ") == std::string::npos ? kChangeH : kRefusedH; }

std::string titleOf(Library& lib, const std::string& key) {
    if (key.empty()) return "";
    if (auto i = lib.zoteroItem(key)) return i->title;
    if (auto i = lib.item(key)) return i->title;
    return "";
}

std::string whatOf(const PluginChange& c, const std::map<std::string, std::string>& collections) {
    auto coll = [&](const std::string& key) {
        auto it = collections.find(key);
        return it == collections.end() ? std::string("a collection") : oneLine(it->second);
    };
    using K = PluginChange::Kind;
    switch (c.kind) {
        case K::CreateItem: return "Add an item";
        case K::AddToCollection: return "File into " + coll(c.value);
        case K::AddTag: return "Add the tag “" + oneLine(c.value) + "”";
        case K::AddNote: return "Add a note";
        case K::Trash: return "Move to the trash";
        case K::RemoveFromCollection: return "Take out of " + coll(c.value);
        case K::RemoveTag: return "Remove the tag “" + oneLine(c.value) + "”";
        case K::DeleteNote: return "Delete a note";
    }
    return "";
}

std::string outcomeOf(const PluginChange& c) {
    using S = PluginChange::State;
    switch (c.state) {
        case S::Pending: return "Pending";
        case S::Sent: return "Sent";
        case S::Refused: return oneLine(c.message).empty() ? "Refused" : "Refused: " + oneLine(c.message);
        case S::Cancelled: return "Cancelled";
        case S::Declined: return "Declined";
    }
    return "";
}

}  // namespace

PluginChangesScreen::PluginChangesScreen(std::string pluginId, std::string pluginName)
    : id_(std::move(pluginId)), name_(std::move(pluginName)) {}

void PluginChangesScreen::onShow(Nav& nav) {
    Library& lib = nav.library();
    std::map<std::string, std::string> collections;
    for (const auto& c : lib.collections()) collections[c.key] = c.name;
    all_.clear();
    for (const auto& c : lib.pluginChangeLog(id_, Library::kPluginChangeLog)) {
        Line line{whatOf(c, collections), "", outcomeOf(c)};
        using K = PluginChange::Kind;
        std::string title;
        if (c.kind == K::AddNote) title = titleOf(lib, c.parentKey);
        else if (c.kind == K::DeleteNote) {
            if (auto n = lib.zoteroNote(c.itemKey)) title = titleOf(lib, n->parentKey);
        } else title = titleOf(lib, c.itemKey);
        if (title.empty() && c.kind == K::CreateItem) {  // withdrawn: what it asked for
            auto v = nlohmann::json::parse(c.value, nullptr, false);
            if (v.is_object() && v.contains("fields") && v["fields"].is_object() && v["fields"].value("title", nlohmann::json()).is_string())
                title = v["fields"]["title"].get<std::string>();
        }
        line.title = oneLine(title);
        all_.push_back(std::move(line));
    }
    // Pages: as many rows as fit above the page indicator.
    starts_.assign(1, 0);
    const int room = H() - kTop - kPageIndicatorH;
    for (size_t i = 0, used = 0; i < all_.size(); ++i) {
        const int h = heightOf(all_[i]);
        if (used + h > static_cast<size_t>(room) && i > starts_.back()) {
            starts_.push_back(i);
            used = 0;
        }
        used += h;
    }
    page_ = std::min(page_, starts_.size() - 1);
}

std::vector<PluginChangesScreen::Line> PluginChangesScreen::lines() const {
    if (all_.empty()) return {};
    const size_t from = starts_[page_], to = page_ + 1 < starts_.size() ? starts_[page_ + 1] : all_.size();
    return {all_.begin() + from, all_.begin() + to};
}

void PluginChangesScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "What " + name_ + " changed", true);
    const int x = kMargin, w = W() - 2 * kMargin;
    const TextStyle what{kSans, 8.5, 500}, title{kSerif, 7.5, 400, false, kSecondary}, state{kSans, 7.5, 400, false, kSecondary},
        why{kSans, 7, 400, false, kSecondary};
    if (all_.empty()) {
        drawText(cr, "Nothing yet. What " + name_ + " asks to change in your Zotero library shows here.", kQuiet,
                 {x, kTop + 20, w, 120}, 2);
        return;
    }
    const auto shown = lines();
    int y = kTop;
    for (size_t i = 0; i < shown.size(); ++i) {
        const Rect r{x, y, w, heightOf(shown[i])};
        y += r.h;
        const Line& l = shown[i];
        const size_t colon = l.outcome.find(": ");
        const std::string word = l.outcome.substr(0, colon), message = colon == std::string::npos ? "" : l.outcome.substr(colon + 2);
        const int stateW = 220;
        drawText(cr, l.what, what, {r.x, r.y + 18, r.w - stateW - 20, 54}, 1);
        drawText(cr, word, word == "Refused" ? TextStyle{kSans, 7.5, 500} : state, {r.x + r.w - stateW, r.y + 24, stateW, 48}, 1,
                 Align::Right);
        drawText(cr, l.title.empty() ? "Not in the library" : l.title, title, {r.x, r.y + 72, r.w, 46}, 1);
        if (!message.empty()) drawText(cr, message, why, {r.x, r.y + 122, r.w, 44}, 1);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
    drawPageIndicator(cr, {0, H() - kPageIndicatorH, W(), kPageIndicatorH}, page_, pages());
}

void PluginChangesScreen::onGesture(const Gesture& g, Nav& nav) {
    const bool turned = g.type == GestureType::Swipe &&
                        ((g.dir == Direction::Up && page_ + 1 < starts_.size() && ++page_) || (g.dir == Direction::Down && page_ > 0 && page_--));
    if (turned) {
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
        return;
    }
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos)) nav.pop();
}

}  // namespace zk
