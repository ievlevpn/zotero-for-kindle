#include "screens/group_libraries.h"

#include <algorithm>

#include "screens/library_sheets.h"
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
constexpr int kGroupRowH = 170, kButtonRowH = 110, kAccessH = 150;
constexpr int kSheetPad = 40, kSheetTitleH = 90, kSheetBodyH = 300, kSheetButtonH = 100;
const TextStyle kLabel{kSans, 8.5, 500}, kValue{kSans, 7.5, 400, false, kSecondary}, kSheetBody{kSans, 8, 400};

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

}  // namespace

std::string groupLibrariesSummary(const std::vector<LibraryInfo>& groups, GroupAccess access) {
    int on = 0, off = 0, gone = 0;
    for (const auto& g : groups) {
        if (g.id == kMyLibrary) continue;
        if (g.gone) ++gone;
        else if (g.enabled) ++on;
        else ++off;
    }
    std::string s;
    auto add = [&](const std::string& part) { s += (s.empty() ? "" : " · ") + part; };
    if (access == GroupAccess::None) add("Your API key can't read group libraries");
    if (on) add(std::to_string(on) + " on");
    if (off) add(std::to_string(off) + " off");
    if (gone) add(std::to_string(gone) + " no longer available");
    return s.empty() ? "None yet: groups you're in appear after a sync" : s;
}

// ---- the sheet -------------------------------------------------------------------------------

namespace {

std::string pending(const LibraryRemoval& c) {
    std::string s;
    if (c.unsent)
        s += c.unsent == 1 ? "1 change made on this Kindle hasn't been sent to Zotero yet."
                           : std::to_string(c.unsent) + " changes made on this Kindle haven't been sent to Zotero yet.";
    if (c.conflicts)
        s += std::string(s.empty() ? "" : " ") +
             (c.conflicts == 1 ? "1 conflict copy is kept on this Kindle." : plural(c.conflicts, "", "conflict copies are kept on this Kindle."));
    return s;
}

}  // namespace

std::string GroupSheet::title() const {
    const std::string name = group.name.empty() ? "this group" : group.name;
    return kind == Kind::TurnOff ? "Turn off " + name + "?" : "Remove " + name + " from this Kindle?";
}

std::string GroupSheet::body() const {
    const std::string waiting = pending(check);
    if (kind == Kind::TurnOff)
        return waiting + " Turned off, the group stops syncing, and they wait until you turn it on again.";
    std::string s = group.gone ? "It's no longer available to you. " : !group.enabled ? "It's off: it no longer syncs and is hidden. " : "";
    if (check.downloaded == 0)
        s += "None of its books are downloaded on this Kindle. Removing deletes its items and highlights here;";
    else
        s += check.downloaded == 1 ? "1 book from it is downloaded on this Kindle. Removing deletes it, and its items and highlights, here;"
                                   : plural(check.downloaded, "", "books from it are downloaded") +
                                         " on this Kindle. Removing deletes them, and its items and highlights, here;";
    s += " nothing changes in Zotero.";
    if (!waiting.empty())
        s += " " + waiting + (group.gone ? " They can't be sent any more: removing loses them." : " Removing loses them.");
    return s;
}

GroupSheet::Choice GroupSheet::primary() const {
    if (kind == Kind::TurnOff) return Choice::SyncFirst;
    if (check.unsent == 0 && check.conflicts == 0) return Choice::Remove;
    return group.gone ? Choice::Keep : Choice::SyncFirst;
}

GroupSheet::Choice GroupSheet::secondary() const {
    if (kind == Kind::TurnOff) return Choice::TurnOff;
    if (check.unsent == 0 && check.conflicts == 0) return Choice::Keep;
    return Choice::RemoveAnyway;
}

std::string GroupSheet::label(Choice c) {
    switch (c) {
        case Choice::SyncFirst: return "Sync first";
        case Choice::TurnOff: return "Turn off";
        case Choice::Remove: return "Remove from Kindle";
        case Choice::RemoveAnyway: return "Remove anyway";
        case Choice::Keep: return "Keep";
        case Choice::None: break;
    }
    return "";
}

// ---- the screen ------------------------------------------------------------------------------

Rect GroupLibrariesScreen::content() const { return {0, kHeaderH + 10, W(), H() - kHeaderH - 10}; }

void GroupLibrariesScreen::onShow(Nav& nav) {
    sheet_.reset();
    load(nav.library());
}

void GroupLibrariesScreen::load(Library& lib) {
    std::vector<Row> was = std::move(rows_);
    rows_.clear();
    access_ = lib.keyGroupAccess();
    for (auto& g : lib.libraries(true)) {
        if (g.id == kMyLibrary) continue;
        Row r;
        r.check = lib.libraryRemovalCheck(g.id);
        r.hasCopy = g.items > 0 || r.check.downloaded > 0;
        r.group = std::move(g);
        for (const auto& w : was)  // still being removed: until the worker's SyncFinished
            if (w.group.id == r.group.id) r.removing = w.removing;
        r.syncing = waiting_ && waiting_->lib == r.group.id;
        rows_.push_back(std::move(r));
    }
    layout();
}

int GroupLibrariesScreen::rowHeight(const Row& r) const { return kGroupRowH + (removeOffered(r) ? kButtonRowH : 0); }

void GroupLibrariesScreen::layout() {
    starts_.assign(1, 0);
    const Rect c = content();
    int used = access_ == GroupAccess::None ? kAccessH : 0;
    const int room = c.h - kPageIndicatorH;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const int h = rowHeight(rows_[i]);
        if (used + h > room && i > starts_.back()) {
            starts_.push_back(i);
            used = 0;
        }
        used += h;
    }
    page_ = std::min(page_, pages() - 1);
}

size_t GroupLibrariesScreen::indexOf(int64_t lib) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].group.id == lib) return i;
    return rows_.size();
}

bool GroupLibrariesScreen::removeOffered(const Row& r) const {
    return !r.removing && !r.syncing && (r.group.gone || (!r.group.enabled && r.hasCopy));
}

std::string GroupLibrariesScreen::note(const Row& r) const {
    if (r.removing) return "Removing from this Kindle…";
    if (r.syncing) return "Syncing first…";
    if (r.group.gone) return std::string("No longer available") + (r.hasCopy ? " · still on this Kindle" : "");
    if (!r.group.enabled) return std::string("Off") + (r.hasCopy ? " · still on this Kindle" : "");
    return withThousands(r.group.items) + (r.group.items == 1 ? " item" : " items") + (r.group.canEdit ? "" : " · read-only");
}

std::optional<Rect> GroupLibrariesScreen::accessLineRect() const {
    if (access_ != GroupAccess::None || page_ != 0) return std::nullopt;
    const Rect c = content();
    return Rect{kMargin, c.y + 10, W() - 2 * kMargin, kAccessH - 30};
}

Rect GroupLibrariesScreen::rowRect(size_t i) const {
    const size_t first = starts_[static_cast<size_t>(page_)];
    const size_t end = page_ + 1 < pages() ? starts_[static_cast<size_t>(page_) + 1] : rows_.size();
    if (i < first || i >= end) return {};
    int y = content().y + (page_ == 0 && access_ == GroupAccess::None ? kAccessH : 0);
    for (size_t k = first; k < i; ++k) y += rowHeight(rows_[k]);
    return {kMargin, y, W() - 2 * kMargin, rowHeight(rows_[i])};
}

Rect GroupLibrariesScreen::toggleRect(size_t i) const {
    const Rect r = rowRect(i);
    if (r.empty() || rows_[i].group.gone || rows_[i].removing) return {};
    return {r.x + r.w - 140, r.y + 40, 140, 80};
}

Rect GroupLibrariesScreen::removeRect(size_t i) const {
    const Rect r = rowRect(i);
    if (r.empty() || !removeOffered(rows_[i])) return {};
    return {r.x, r.y + kGroupRowH - 20, 440, 84};
}

Rect GroupLibrariesScreen::sheetRect() const {
    // As tall as its text (up to 6 lines).
    const int body = sheet_ ? std::clamp(measureText(sheet_->body(), kSheetBody, W() - 2 * kMargin, 6).height + 10, 60, kSheetBodyH)
                            : kSheetBodyH;
    const int h = kSheetPad + kSheetTitleH + body + 40 + kSheetButtonH + kSheetPad;
    return {0, H() - h, W(), h};
}

Rect GroupLibrariesScreen::sheetButtonRect(int i) const {
    const Rect s = sheetRect();
    const int w = (s.w - 2 * kMargin - 40) / 2;
    const int y = s.y + s.h - kSheetPad - kSheetButtonH;
    return i == 0 ? Rect{s.x + s.w - kMargin - w, y, w, kSheetButtonH} : Rect{s.x + kMargin, y, w, kSheetButtonH};
}

void GroupLibrariesScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Group libraries", true);
    if (auto a = accessLineRect()) drawText(cr, kNoGroupAccess, kValue, *a, 3);
    if (rows_.empty() && access_ != GroupAccess::None)
        drawText(cr, "No group libraries yet. Groups you're in appear here after the next sync.", {kSerif, 10, 400, false, kSecondary},
                 {kMargin + 40, 500, W() - 2 * kMargin - 80, 200}, 3, Align::Center);
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Rect r = rowRect(i);
        if (r.empty()) continue;
        const Row& row = rows_[i];
        const int textW = r.w - 180;
        drawText(cr, row.group.name.empty() ? "Untitled group" : row.group.name, kLabel, {r.x, r.y + 30, textW, 50}, 1);
        drawText(cr, note(row), kValue, {r.x, r.y + 86, textW, 50}, 1);
        if (const Rect t = toggleRect(i); !t.empty()) drawToggle(cr, t, row.group.enabled);
        if (const Rect b = removeRect(i); !b.empty()) drawButton(cr, b, "Remove from Kindle", false);
        hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
    }
    if (pages() > 1) drawPageIndicator(cr, {0, H() - kPageIndicatorH, W(), kPageIndicatorH}, page_, pages());
    if (!sheet_) return;
    cairo_save(cr);  // fade what's behind, so the sheet reads as on top (as the Library's sheets)
    cairo_rectangle(cr, 0, kHeaderH, W(), H() - kHeaderH);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
    cairo_fill(cr);
    cairo_restore(cr);
    const Rect s = sheetRect();
    fillRect(cr, s, 255);
    fillRect(cr, {s.x, s.y, s.w, 4}, kInk);
    drawText(cr, sheet_->title(), {kSans, 11, 700}, {s.x + kMargin, s.y + kSheetPad, s.w - 2 * kMargin, kSheetTitleH - 10}, 1);
    drawText(cr, sheet_->body(), kSheetBody, {s.x + kMargin, s.y + kSheetPad + kSheetTitleH, s.w - 2 * kMargin, kSheetBodyH}, 6);
    drawButton(cr, sheetButtonRect(1), GroupSheet::label(sheet_->secondary()), false);
    drawButton(cr, sheetButtonRect(0), GroupSheet::label(sheet_->primary()), true);
}

void GroupLibrariesScreen::toggle(size_t i, Nav& nav) {
    Library& lib = nav.library();
    const Row row = rows_[i];
    if (!row.group.enabled) {  // on: it syncs again (downloaded again if its copy was removed)
        lib.setLibraryEnabled(row.group.id, true);
        nav.sync().syncNow();
        load(lib);
        nav.damage(content(), Waveform::Ui);
        return;
    }
    const LibraryRemoval check = lib.libraryRemovalCheck(row.group.id);
    if (check.unsent > 0 || check.conflicts > 0) {  // ask first: off, they would wait
        sheet_ = GroupSheet{GroupSheet::Kind::TurnOff, row.group, check};
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Ui);
        return;
    }
    lib.setLibraryEnabled(row.group.id, false);  // off at once: not synced, hidden everywhere
    load(lib);
    if (row.hasCopy) {  // and its copy? Only on a yes.
        GroupSheet s{GroupSheet::Kind::Remove, row.group, check};
        s.group.enabled = false;
        sheet_ = std::move(s);
    }
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Ui);
}

void GroupLibrariesScreen::askRemove(size_t i, Nav& nav) {
    const Row row = rows_[i];
    const LibraryRemoval check = nav.library().libraryRemovalCheck(row.group.id);
    if (!row.hasCopy && check.unsent == 0 && check.conflicts == 0) {  // nothing of it here: just forgotten
        rows_[i].removing = true;
        nav.sync().removeLibrary(row.group.id, false);
        layout();
        nav.damage(content(), Waveform::Ui);
        return;
    }
    sheet_ = GroupSheet{GroupSheet::Kind::Remove, row.group, check};
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Ui);
}

void GroupLibrariesScreen::choose(GroupSheet::Choice c, Nav& nav) {
    const GroupSheet s = *sheet_;
    sheet_.reset();
    Library& lib = nav.library();
    const int64_t id = s.group.id;
    switch (c) {
        case GroupSheet::Choice::SyncFirst: {
            // Its changes go with the next sync; a group switched off is on for it, then off again.
            const bool reEnable = s.kind == GroupSheet::Kind::Remove && !s.group.gone && !s.group.enabled;
            if (reEnable) lib.setLibraryEnabled(id, true);
            waiting_ = Waiting{id, s.kind, reEnable};
            nav.sync().syncNow();
            break;
        }
        case GroupSheet::Choice::TurnOff: lib.setLibraryEnabled(id, false); break;
        case GroupSheet::Choice::Remove:
        case GroupSheet::Choice::RemoveAnyway:
            if (!s.group.gone && lib.libraryInfo(id).value_or(LibraryInfo{}).enabled) lib.setLibraryEnabled(id, false);
            nav.sync().removeLibrary(id, c == GroupSheet::Choice::RemoveAnyway);
            if (size_t i = indexOf(id); i < rows_.size()) rows_[i].removing = true;
            break;
        case GroupSheet::Choice::Keep:
        case GroupSheet::Choice::None: break;
    }
    load(lib);
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);  // the sheet goes: no ghost of it
}

void GroupLibrariesScreen::afterSync(Nav& nav) {
    const Waiting w = *waiting_;
    waiting_.reset();
    Library& lib = nav.library();
    auto info = lib.libraryInfo(w.lib);
    if (!info) return;
    const LibraryRemoval check = lib.libraryRemovalCheck(w.lib);
    const bool clean = check.unsent == 0 && check.conflicts == 0;
    if (w.kind == GroupSheet::Kind::TurnOff && !clean) {  // still unsent (no Wi-Fi?): ask again
        sheet_ = GroupSheet{GroupSheet::Kind::TurnOff, *info, check};
        return;
    }
    // Sent: off now (as the user asked), then the copy only on a yes.
    if (info->enabled && (w.kind == GroupSheet::Kind::TurnOff || w.reEnabled || clean)) lib.setLibraryEnabled(w.lib, false);
    info->enabled = false;
    if (clean && info->items == 0 && check.downloaded == 0) return;  // nothing of it here
    sheet_ = GroupSheet{GroupSheet::Kind::Remove, *info, check};
}

void GroupLibrariesScreen::onGesture(const Gesture& g, Nav& nav) {
    if (sheet_) {
        if (g.type != GestureType::Tap) return;
        if (sheetButtonRect(0).contains(g.pos)) return choose(sheet_->primary(), nav);
        if (sheetButtonRect(1).contains(g.pos)) return choose(sheet_->secondary(), nav);
        if (!sheetRect().contains(g.pos)) return choose(GroupSheet::Choice::Keep, nav);  // a tap outside: nothing happens
        return;
    }
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up || g.dir == Direction::Down)) {
        const int to = std::clamp(page_ + (g.dir == Direction::Up ? 1 : -1), 0, pages() - 1);
        if (to != page_) {
            page_ = to;
            nav.damage(content(), Waveform::Quality);
        }
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (const Rect t = toggleRect(i); !t.empty() && t.contains(g.pos)) return toggle(i, nav);
        if (const Rect b = removeRect(i); !b.empty() && b.contains(g.pos)) return askRemove(i, nav);
    }
}

void GroupLibrariesScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished) return;
    for (auto& r : rows_) r.removing = false;  // the worker's removal ends with one too
    if (waiting_) afterSync(nav);
    load(nav.library());
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Quality);
}

}  // namespace zk
