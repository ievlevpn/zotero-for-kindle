#include "screens/item_details.h"

#include "screens/item_sheet.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

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

void ItemDetailsScreen::onShow(Nav& nav) {
    if (auto d = nav.library().details(row_.key)) details_ = std::move(*d);
    if (auto r = nav.library().rowFor(row_.key)) row_ = std::move(*r);
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
    std::string tags;
    for (const auto& t : item.tags) tags += (tags.empty() ? "" : ", ") + t;
    section("Tags", tags, 2);

    // Files, each with its own Remove from Kindle.
    removeTargets_.clear();
    if (!details_.files.empty()) {
        drawText(cr, details_.files.size() == 1 ? "File" : "Files", label, {x, y, w, 40}, 1);
        y += 50;
        for (const auto& f : details_.files) {
            y += drawText(cr, fileLine(f, details_.progress), value, {x, y, w, 110}, 2).height + 14;
            if (f.onKindle) {
                const Rect r{x, y, 440, 84};
                drawButton(cr, r, confirming_ == f.attachment.key ? "Tap again to remove" : "Remove from Kindle", false);
                removeTargets_.push_back({f.attachment.key, r});
                y += 84 + 14;
            }
            y += 16;
        }
        y += 12;
    }
    if (!details_.abstract.empty()) {
        drawText(cr, "Abstract", label, {x, y, w, 40}, 1);
        y += 44;
        const int bottom = openRect().y - 40;
        if (bottom - y > 50) drawText(cr, details_.abstract, body, {x, y, w, bottom - y}, 1000);
    }
    if (!details_.files.empty() && open_) drawButton(cr, openRect(), "Open", true);
}

void ItemDetailsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (!details_.files.empty() && open_ && openRect().contains(g.pos)) {
        open_(row_);
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
