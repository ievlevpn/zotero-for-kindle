#include "screens/item_details.h"

#include "screens/item_sheet.h"
#include "plugins/identifiers.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cctype>

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }

std::string roleLabel(const std::string& type) {
    if (type.empty() || type == "author") return "";
    std::string t = itemTypeLabel(type);  // "seriesEditor" -> "Series editor"
    t[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
    return " (" + t + ")";
}

}  // namespace

ItemDetailsScreen::ItemDetailsScreen(ItemRow row, std::function<void(const ItemRow&)> open)
    : row_(std::move(row)), open_(std::move(open)) {}

Rect ItemDetailsScreen::openRect() { return {kMargin, H() - kMargin - 100, 360, 100}; }

std::vector<Rect> ItemDetailsScreen::removeRects() const {
    std::vector<Rect> out;
    for (const auto& [key, r] : removeTargets_) out.push_back(r);
    return out;
}

std::vector<Rect> ItemDetailsScreen::fileOpenRects() const {
    std::vector<Rect> out;
    for (const auto& [key, r] : openTargets_) out.push_back(r);
    return out;
}

std::vector<Rect> ItemDetailsScreen::actionRects() const { return actionRects_; }

void ItemDetailsScreen::onShow(Nav& nav) {
    if (auto d = nav.library().details(row_.key)) details_ = std::move(*d);
    library_ = details_.item.lib != kMyLibrary ? nav.library().libraryName(details_.item.lib) : "";
    if (auto r = nav.library().rowFor(row_.key)) row_ = std::move(*r);
    PluginEntries* e = nav.pluginEntries();
    entriesVersion_ = e ? e->version() : 0;
    if (!result_) actions_ = e ? e->actions("itemAction") : std::vector<PluginAction>{};
}

void ItemDetailsScreen::onTick(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    if (e && !result_ && e->version() != entriesVersion_) {  // plugins loaded, turned on or off meanwhile
        entriesVersion_ = e->version();
        auto now = e->actions("itemAction");
        bool same = now.size() == actions_.size();
        for (size_t i = 0; same && i < now.size(); ++i) same = now[i].pluginId == actions_[i].pluginId && now[i].id == actions_[i].id;
        actions_ = std::move(now);
        if (!same) nav.damage({0, 0, W(), H()}, Waveform::Ui);
    }
    if (!result_) return;
    const bool working = result_->running() && !result_->shown();
    if (result_->tick(nav) == PluginResult::Status::Closed) result_.reset();
    const bool now = result_ && result_->running() && !result_->shown();
    if (working != now && running_ >= 0 && running_ < static_cast<int>(actionRects_.size()))
        nav.damage(actionRects_[static_cast<size_t>(running_)], Waveform::Ui);
    if (!result_) running_ = -1;
}

void ItemDetailsScreen::onClose(Nav&) {
    if (result_) result_->stop();
}

void ItemDetailsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "About this item", true);
    const TextStyle title{kSerif, 14, 700}, label{kSans, 7, 500, false, kSecondary}, value{kSans, 8, 400},
        body{kSerif, 8, 400};
    const int x = kMargin, w = W() - 2 * kMargin;
    int y = kHeaderH + 20;
    const auto& item = details_.item;
    y += drawText(cr, item.title.empty() ? "Untitled" : item.title, title, {x, y, w, 220}, 3).height + 24;

    auto section = [&](const std::string& name, const std::string& text, int maxLines) {
        if (text.empty()) return;
        drawText(cr, name, label, {x, y, w, 40}, 1);
        y += 44;
        y += drawText(cr, text, value, {x, y, w, maxLines * 52}, maxLines).height + 22;
    };
    std::string people;
    for (const auto& c : item.creators) {
        const std::string name = !c.name.empty() ? c.name : c.first.empty() ? c.last : c.first + " " + c.last;
        people += (people.empty() ? "" : "\n") + name + roleLabel(c.type);
    }
    section(item.creators.size() == 1 ? "Author" : "People", people, 4);
    std::string kind = itemTypeLabel(item.itemType);
    if (!item.date.empty()) kind += " · " + item.date;
    section("Type", kind, 1);
    section("Published in", item.publication, 2);
    std::string collections;
    for (const auto& c : details_.collectionPaths) collections += (collections.empty() ? "" : "\n") + c;
    section("Collections", collections, 3);
    if (item.lib != kMyLibrary) section("Library", library_, 1);  // a group's item
    std::string tags;
    for (const auto& t : item.tags) tags += (tags.empty() ? "" : ", ") + t;
    section("Tags", tags, 2);

    // Files, each with its own Open and Remove from Kindle.
    removeTargets_.clear();
    openTargets_.clear();
    if (!details_.files.empty()) {
        drawText(cr, details_.files.size() == 1 ? "File" : "Files", label, {x, y, w, 40}, 1);
        y += 50;
        auto readable = [](const AttachmentRecord& a) {
            return isReadableType(a.contentType) && a.linkMode != "linked_file" && a.linkMode != "linked_url";
        };
        const auto readables = std::count_if(details_.files.begin(), details_.files.end(),
                                             [&](const ItemDetails::File& f) { return readable(f.attachment); });
        for (const auto& f : details_.files) {
            y += drawText(cr, fileLine(f, f.progress), value, {x, y, w, 110}, 2).height + 14;
            const auto& a = f.attachment;
            int bx = x;
            if (readables > 1 && readable(a) && open_) {  // one file: the page's Open is the file's
                const Rect r{bx, y, 200, 84};
                drawButton(cr, r, "Open", false);
                openTargets_.push_back({a.key, r});
                bx += 200 + 24;
            }
            if (f.onKindle) {
                const Rect r{bx, y, 440, 84};
                drawButton(cr, r, confirming_ == f.attachment.key ? "Tap again to remove" : "Remove from Kindle", false);
                removeTargets_.push_back({f.attachment.key, r});
            }
            if (bx > x || f.onKindle) y += 84 + 14;
            y += 16;
        }
        y += 12;
    }
    // The plugins' item actions: title, and the plugin's name (or "Working…") on the right.
    actionRects_.clear();
    if (!actions_.empty()) {
        drawText(cr, "Plugins", label, {x, y, w, 40}, 1);
        y += 50;
        const TextStyle quiet{kSans, 7, 400, false, kSecondary};
        for (size_t i = 0; i < actions_.size(); ++i) {
            const Rect r{x, y, w, 84};
            const bool working = static_cast<int>(i) == running_ && result_ && result_->running() && !result_->shown();
            const std::string side = working ? "Working…" : actions_[i].pluginName;
            const int sideW = std::min(measureText(side, quiet, w / 2, 1).width + 10, w / 2);
            centredText(cr, actions_[i].title, value, {r.x, r.y, r.w - sideW - 30, r.h}, Align::Left);
            centredText(cr, side, quiet, {r.x + r.w - sideW, r.y, sideW, r.h}, Align::Right);
            hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
            actionRects_.push_back(r);
            y += 84;
        }
        y += 28;
    }
    if (!details_.abstract.empty()) {
        drawText(cr, "Abstract", label, {x, y, w, 40}, 1);
        y += 44;
        const int bottom = openRect().y - 40;
        if (bottom - y > 50) drawText(cr, details_.abstract, body, {x, y, w, bottom - y}, 1000);
    }
    if (!details_.files.empty() && open_) drawButton(cr, openRect(), "Open", true);
    if (result_) result_->draw(cr);
}

void ItemDetailsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (result_ && result_->shown()) {  // the result pop-up takes every gesture until it is closed
        if (result_->onGesture(g, nav) == PluginResult::Status::Closed) {
            result_.reset();
            running_ = -1;
        }
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (!details_.files.empty() && open_ && openRect().contains(g.pos)) {
        open_(row_);
        return;
    }
    for (size_t i = 0; i < actionRects_.size() && i < actions_.size(); ++i)
        if (actionRects_[i].contains(g.pos)) {
            if (result_) {  // one run at a time; its "Working…" row tapped again stops it
                if (static_cast<int>(i) == running_) {
                    result_->stop();
                    result_.reset();
                    running_ = -1;
                    nav.damage(actionRects_[i], Waveform::Ui);
                }
                return;
            }
            PluginEntries* e = nav.pluginEntries();
            auto item = nav.library().item(row_.key);
            if (!e || !item) return;
            result_ = std::make_unique<PluginResult>(*e, actions_[i], "itemAction",
                                                     plugins::itemJson(*item, nav.library().isRead(row_.key), nav.library().libraryName(item->lib)), Rect{0, 0, W(), H()});
            running_ = static_cast<int>(i);
            nav.damage(actionRects_[i], Waveform::Ui);
            return;
        }
    for (const auto& [key, r] : openTargets_)
        if (r.contains(g.pos)) {
            if (auto row = nav.library().rowFor(row_.key, key)) open_(*row);
            return;
        }
    for (const auto& [key, r] : removeTargets_)
        if (r.contains(g.pos)) {
            if (confirming_ != key) {
                confirming_ = key;
                nav.damage(r, Waveform::Ui);
                return;
            }
            confirming_.clear();
            nav.library().removeDownload(key);
            onShow(nav);
            nav.damage({0, 0, W(), H()}, Waveform::Ui);
            return;
        }
    if (!confirming_.empty()) {
        confirming_.clear();
        nav.damage({0, 0, W(), H()}, Waveform::Ui);
    }
}

}  // namespace zk
