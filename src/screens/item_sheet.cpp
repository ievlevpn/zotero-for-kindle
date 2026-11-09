#include "screens/item_sheet.h"

#include <cctype>
#include <cstdio>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kPad = 40, kButtonH = 100, kButtonGap = 24, kNarrowButtonW = 260, kAbstractLines = 4;
const TextStyle kTitle{theme::kSerif, 12, 700}, kMeta{theme::kSans, 7.5, 400, false, theme::kSecondary},
    kBody{theme::kSerif, 8, 400};

std::string personName(const Creator& c) {
    if (!c.name.empty()) return c.name;
    return c.first.empty() ? c.last : c.first + " " + c.last;
}

std::string megabytes(int64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024 * 1024));
    return buf;
}

}  // namespace

std::string itemTypeLabel(const std::string& itemType) {
    if (itemType.empty() || itemType == "attachment") return "File";
    std::string s;
    for (char c : itemType) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            s += ' ';
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            s += c;
        }
    }
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string peopleLine(const ItemRecord& item, int max) {
    std::vector<std::string> names;
    for (const auto& c : item.creators) names.push_back(personName(c));
    if (names.empty()) return "";
    if (static_cast<int>(names.size()) > max) return names[0] + " et al.";
    std::string s = names[0];
    for (size_t i = 1; i < names.size(); ++i) s += (i + 1 == names.size() ? " and " : ", ") + names[i];
    return s;
}

std::string fileLine(const ItemDetails::File& f, double progress) {
    const auto& a = f.attachment;
    std::string s = a.contentType == "application/epub+zip" ? "EPUB"
                    : a.contentType == "application/pdf"    ? "PDF"
                    : a.filename.empty()                     ? "File"
                                                             : a.filename;
    if (a.linkMode == "linked_file") return s + " · linked file (not synced)";
    if (f.onKindle) s += " · " + megabytes(f.bytes) + " · on this Kindle";
    else s += " · in Zotero";
    if (progress >= 0) s += " · " + std::to_string(static_cast<int>(progress * 100 + 0.5)) + "% read";
    if (f.highlights > 0) s += " · " + std::to_string(f.highlights) + (f.highlights == 1 ? " highlight" : " highlights");
    return s;
}

ItemSheet::ItemSheet(ItemRow row, ItemDetails details) : row_(std::move(row)), details_(std::move(details)) {}

const ItemDetails::File* ItemSheet::file() const {
    for (const auto& f : details_.files)
        if (f.attachment.key == row_.attachmentKey) return &f;
    return details_.files.empty() ? nullptr : &details_.files.front();
}

// The sheet is as tall as what it shows: title, three lines, the abstract's start, the buttons.
int ItemSheet::contentHeight(int width) const {
    const int w = width - 2 * kMargin;
    int h = kPad + measureText(details_.item.title.empty() ? "Untitled" : details_.item.title, kTitle, w, 2).height + 14;
    h += 3 * 52 + 18;
    if (!details_.abstract.empty()) h += measureText(details_.abstract, kBody, w, kAbstractLines).height + 30;
    return h + kButtonH + kButtonGap + kButtonH + kPad;
}

Rect ItemSheet::sheetRect(Rect area) const {
    const int h = std::min(contentHeight(area.w), area.h);
    return {area.x, area.y + area.h - h, area.w, h};
}

// Open and More… narrow, Remove from Kindle (and "Tap again to remove") wide between them.
Rect ItemSheet::openRect(Rect area) const {
    const Rect s = sheetRect(area);
    return {s.x + kMargin, s.y + s.h - kPad - kButtonH, kNarrowButtonW, kButtonH};
}

Rect ItemSheet::removeRect(Rect area) const {
    const Rect o = openRect(area), m = moreRect(area);
    return {o.x + o.w + 30, o.y, m.x - 30 - (o.x + o.w + 30), o.h};
}

Rect ItemSheet::moreRect(Rect area) const {
    const Rect s = sheetRect(area);
    return {s.x + s.w - kMargin - kNarrowButtonW, s.y + s.h - kPad - kButtonH, kNarrowButtonW, kButtonH};
}

Rect ItemSheet::readRect(Rect area) const {
    const Rect o = openRect(area), m = moreRect(area);
    return {o.x, o.y - kButtonGap - kButtonH, m.x + m.w - o.x, kButtonH};
}

void ItemSheet::draw(cairo_t* cr, Rect area) const {
    // Fade what's behind, so the sheet reads as on top.
    cairo_save(cr);
    cairo_rectangle(cr, area.x, area.y, area.w, area.h);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
    cairo_fill(cr);
    cairo_restore(cr);

    const Rect s = sheetRect(area);
    fillRect(cr, s, 255);
    fillRect(cr, {s.x, s.y, s.w, 4}, kInk);
    const int x = s.x + kMargin, w = s.w - 2 * kMargin;
    int y = s.y + kPad;
    const TextStyle &title = kTitle, &meta = kMeta, &body = kBody;
    y += drawText(cr, details_.item.title.empty() ? "Untitled" : details_.item.title, title, {x, y, w, 140}, 2).height + 14;

    std::string people = peopleLine(details_.item, 3);
    std::string line1 = people;
    for (const std::string& part : {details_.item.year, itemTypeLabel(details_.item.itemType)})
        if (!part.empty()) line1 += (line1.empty() ? "" : " · ") + part;
    drawText(cr, line1, meta, {x, y, w, 50}, 1);
    y += 52;
    std::string line2 = details_.item.publication;
    for (const auto& c : details_.collectionPaths) line2 += (line2.empty() ? "" : " · ") + c;
    drawText(cr, line2, meta, {x, y, w, 50}, 1);
    y += 52;
    const ItemDetails::File* f = file();
    drawText(cr, f ? fileLine(*f, details_.progress) : "No file", meta, {x, y, w, 50}, 1);
    y += 52 + 18;
    const int abstractBottom = readRect(area).y - 30;
    if (!details_.abstract.empty() && abstractBottom - y > 60)
        drawText(cr, details_.abstract, body, {x, y, w, abstractBottom - y}, kAbstractLines);

    drawButton(cr, readRect(area), row_.read ? "Mark as unread" : "Mark as read", false);
    drawButton(cr, openRect(area), "Open", true);
    if (!f) {  // nothing to open: greyed out
        const Rect o = openRect(area);
        cairo_save(cr);
        cairo_rectangle(cr, o.x, o.y, o.w, o.h);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    if (f && f->onKindle) drawButton(cr, removeRect(area), confirmRemove_ ? "Tap again to remove" : "Remove from Kindle", false);
    drawButton(cr, moreRect(area), "More…", false);
}

ItemSheet::Action ItemSheet::onGesture(const Gesture& g, Rect area, Nav& nav) {
    if (g.type != GestureType::Tap) return Action::None;
    if (!sheetRect(area).contains(g.pos)) return Action::Close;
    const ItemDetails::File* f = file();
    if (openRect(area).contains(g.pos)) return f ? Action::Open : Action::None;
    if (moreRect(area).contains(g.pos)) return Action::More;
    if (readRect(area).contains(g.pos)) {
        nav.library().markRead(row_.key, !row_.read);
        plugins::itemMarked(nav.plugins(), nav.library(), row_.key, !row_.read);
        nav.sync().pushSoon();
        if (auto fresh = nav.library().rowFor(row_.key)) row_ = std::move(*fresh);
        confirmRemove_ = false;
        return Action::Changed;
    }
    if (f && f->onKindle && removeRect(area).contains(g.pos)) {
        if (!confirmRemove_) {
            confirmRemove_ = true;
            nav.damage(removeRect(area), Waveform::Ui);
            return Action::None;
        }
        confirmRemove_ = false;
        nav.library().removeDownload(f->attachment.key);
        if (auto fresh = nav.library().details(row_.key)) details_ = std::move(*fresh);
        if (auto fresh = nav.library().rowFor(row_.key)) row_ = std::move(*fresh);
        return Action::Changed;
    }
    if (confirmRemove_) {  // anywhere else on the sheet: never mind
        confirmRemove_ = false;
        nav.damage(removeRect(area), Waveform::Ui);
    }
    return Action::None;
}

}  // namespace zk
