#include "screens/plugin_page.h"

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>

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
constexpr int kActionH = 100;  // a repair at the bottom: one line of text between rules

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
    if (PluginHost* h = host_ ? host_() : nullptr)
        for (auto& s : h->status())
            if (s.folder.dir == id_) status_ = std::move(s);
    pending_ = static_cast<int>(nav.library().pendingPluginChanges(id_).size());
}

void PluginPageScreen::onShow(Nav& nav) {
    load(nav);
    ask_ = Ask::None;
}

std::string PluginPageScreen::name() const { return status_ ? pluginName(*status_) : "Plugin"; }

std::vector<std::string> PluginPageScreen::permissionLines() const {
    std::vector<std::string> out;
    if (!status_ || !status_->folder.manifest) return out;
    const PluginManifest& m = *status_->folder.manifest;
    for (Permission p : m.permissions) {
        if (p != Permission::Net) {
            out.push_back(permissionText(p));
            continue;
        }
        for (const auto& host : m.hosts)
            out.push_back(host.rfind("*.", 0) == 0 ? "Connects to sites under " + host.substr(2) : permissionText(p, host));
    }
    return out;
}

std::vector<std::string> PluginPageScreen::errorLines() const {
    std::vector<std::string> out;
    if (!status_) return out;
    for (auto it = status_->errors.rbegin(); it != status_->errors.rend() && out.size() < 3; ++it) out.push_back(oneLine(*it));
    return out;
}

std::string PluginPageScreen::turnLabel() const {
    if (!status_ || !status_->folder.manifest || !status_->folder.problem.empty()) return "";
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
    if (status_) actions.push_back(&l.remove);
    const int actionsTop = H() - 30 - static_cast<int>(actions.size()) * kActionH;
    for (size_t i = 0; i < actions.size(); ++i) *actions[i] = {x, actionsTop + static_cast<int>(i) * kActionH, w, kActionH};
    const int bottom = actionsTop - 30;  // what's above stops here

    int y = kTop;
    auto room = [&](int lineH) { return std::max(0, (bottom - y) / lineH); };
    if (!status_) {
        text(cr, host_ && host_() ? "This plugin isn't installed any more." : "Plugins aren't available right now.", kQuiet,
             {x, y + 10, w, 120}, 2);
        return l;
    }
    const PluginStatus& s = *status_;
    if (manifest) {
        const PluginManifest& m = *s.folder.manifest;
        std::string by = "Version " + oneLine(m.version);
        if (!oneLine(m.author).empty()) by += " · " + oneLine(m.author);
        y += text(cr, by, kByline, {x, y, w, 50}, 1) + 24;
        if (!oneLine(m.description).empty())
            y += text(cr, m.description, kAbout, {x, y, w, bottom - y}, std::min(4, room(52))) + 24;
        if (s.enabled && !oneLine(s.badge).empty())  // what it says of itself now
            y += text(cr, oneLine(s.badge), kQuiet, {x, y, w, 50}, std::min(1, room(50))) + 24;
        y += 6;
    }
    if (!s.folder.problem.empty() || s.stopped || !s.problem.empty())
        y += text(cr, pluginNote(s), kAlarm, {x, y, w, bottom - y}, std::min(3, room(52))) + 30;

    if (manifest) {
        if (y + 44 < bottom) y += text(cr, "What it can do", kLabel, {x, y, w, 44}, 1) + 14;
        auto lines = permissionLines();
        if (lines.empty()) lines.push_back("Nothing that needs your permission");
        for (const auto& line : lines) {
            if (room(52) == 0) break;
            y += text(cr, line, kLine, {x, y, w, bottom - y}, std::min(2, room(52))) + 8;
        }
        y += 20;
    }
    if (!turnLabel().empty() && y + 92 <= bottom) {
        l.turn = {x, y, 360, 92};
        if (cr) drawButton(cr, l.turn, turnLabel(), !s.enabled);
        y += 92 + 40;
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
        row(l.remove, "Remove", false);
    }
    return l;
}

Rect PluginPageScreen::turnRect() const { return layout().turn; }
Rect PluginPageScreen::changesRect() const { return layout().changes; }
Rect PluginPageScreen::resetRect() const { return layout().reset; }
Rect PluginPageScreen::discardRect() const { return layout().discard; }
Rect PluginPageScreen::removeRect() const { return layout().remove; }

std::string PluginPageScreen::question() const {
    const std::string n = name();
    switch (ask_) {
        case Ask::None: return "";
        case Ask::Reset: return "Reset " + n + "’s data? What it stored on the Kindle is deleted; its changes in Zotero stay.";
        case Ask::Discard:
            return "Discard " + count(pending_, "change ", "changes ") + n + " hasn't sent to Zotero yet? " +
                   (pending_ == 1 ? "It won't reach Zotero." : "They won't reach Zotero.");
        case Ask::Remove:
            return "Remove " + n + "? Its folder is deleted." +
                   (pending_ > 0 ? " " + count(pending_, "change", "changes") + " not synced yet will still be sent." : "");
    }
    return "";
}

std::optional<PluginConfirm> PluginPageScreen::confirm() const {
    if (ask_ == Ask::None) return std::nullopt;
    const char* action = ask_ == Ask::Reset ? "Reset" : ask_ == Ask::Discard ? "Discard" : "Remove";
    return PluginConfirm(name(), {question()}, action);
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
    if (!l.changes.empty() && l.changes.contains(g.pos)) {
        nav.push(std::make_unique<PluginChangesScreen>(id_, name()));
        return;
    }
    const Ask ask = !l.reset.empty() && l.reset.contains(g.pos)       ? Ask::Reset
                    : !l.discard.empty() && l.discard.contains(g.pos) ? Ask::Discard
                    : !l.remove.empty() && l.remove.contains(g.pos)   ? Ask::Remove
                                                                       : Ask::None;
    if (ask == Ask::None) return;
    load(nav);  // the count it states is the count now
    ask_ = ask;
    nav.damage(whole, Waveform::Ui);
}

// ---- What it changed -----------------------------------------------------------------------

namespace {

constexpr int kChangeH = 170;

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

int PluginChangesScreen::perPage() const { return std::max(1, (H() - kTop - kPageIndicatorH) / kChangeH); }

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
    const int64_t offset = pager_.offset();
    pager_ = Pager(static_cast<int64_t>(all_.size()), perPage());
    while (pager_.offset() < offset && pager_.next()) {
    }
}

std::vector<PluginChangesScreen::Line> PluginChangesScreen::lines() const {
    const auto from = all_.begin() + std::min<int64_t>(pager_.offset(), all_.size());
    return {from, from + std::min<int64_t>(perPage(), all_.end() - from)};
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
    for (size_t i = 0; i < shown.size(); ++i) {
        const Rect r{x, kTop + static_cast<int>(i) * kChangeH, w, kChangeH};
        const Line& l = shown[i];
        const size_t colon = l.outcome.find(": ");
        const std::string word = l.outcome.substr(0, colon), message = colon == std::string::npos ? "" : l.outcome.substr(colon + 2);
        const int stateW = 220;
        drawText(cr, l.what, what, {r.x, r.y + 18, r.w - stateW - 20, 54}, 1);
        drawText(cr, word, word == "Refused" ? TextStyle{kSans, 7.5, 500} : state, {r.x + r.w - stateW, r.y + 24, stateW, 48}, 1,
                 Align::Right);
        drawText(cr, l.title.empty() ? "Not in the library" : l.title, title, {r.x, r.y + 72, r.w, 46}, 1);
        if (!message.empty()) drawText(cr, message, why, {r.x, r.y + 120, r.w, 40}, 1);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
    drawPageIndicator(cr, {0, H() - kPageIndicatorH, W(), kPageIndicatorH}, pager_.page(), pager_.pages());
}

void PluginChangesScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up ? pager_.next() : g.dir == Direction::Down && pager_.prev())) {
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
        return;
    }
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos)) nav.pop();
}

}  // namespace zk
