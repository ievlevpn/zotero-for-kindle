#include "screens/file_choice.h"

#include <algorithm>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kPad = 40, kTitleH = 90, kFileH = 150;
const TextStyle kSheetTitle{kSans, 11, 700}, kName{kSerif, 9.5, 700}, kMeta{kSans, 7.5, 400, false, kSecondary},
    kLastRead{kSans, 7, 700, false, kInk};

}  // namespace

FileChoiceScreen::FileChoiceScreen(ItemRow row, std::vector<FileChoice> files, Screen* under, Choose choose)
    : row_(std::move(row)), files_(std::move(files)), under_(under), choose_(std::move(choose)) {}

Rect FileChoiceScreen::area() const {
    return {0, 0, metrics().width, metrics().height - (showsTabBar() ? kTabBarH : 0)};
}

Rect FileChoiceScreen::sheetRect() const {
    const Rect a = area();
    const int fit = std::max(1, (a.h - kHeaderH - 2 * kPad - kTitleH) / kFileH);
    const int n = std::min(static_cast<int>(files_.size()), fit);
    const int h = 2 * kPad + kTitleH + n * kFileH;
    return {a.x, a.y + a.h - h, a.w, h};
}

std::vector<Rect> FileChoiceScreen::fileRects() const {
    const Rect s = sheetRect();
    std::vector<Rect> out;
    for (int y = s.y + kPad + kTitleH; out.size() < files_.size() && y + kFileH <= s.y + s.h - kPad + 1; y += kFileH)
        out.push_back({s.x + kMargin, y, s.w - 2 * kMargin, kFileH});
    return out;
}

std::string FileChoiceScreen::fileMeta(const FileChoice& f) {
    std::string s = f.attachment.contentType == "application/pdf" ? "PDF" : "EPUB";
    if (f.onKindle) s += " · On Kindle";
    if (f.progress > 0) s += " · " + std::to_string(static_cast<int>(f.progress * 100 + 0.5)) + "%";
    return s;
}

void FileChoiceScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    const Rect a = area();
    if (under_) under_->draw(canvas);
    else fillRect(cr, {0, 0, metrics().width, metrics().height}, 255);
    cairo_save(cr);  // fade what's behind, so the sheet reads as on top
    cairo_rectangle(cr, a.x, a.y, a.w, a.h);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
    cairo_fill(cr);
    cairo_restore(cr);
    const Rect s = sheetRect();
    fillRect(cr, s, 255);
    fillRect(cr, {s.x, s.y, s.w, 4}, kInk);
    drawText(cr, "Open which file?", kSheetTitle, {s.x + kMargin, s.y + kPad, s.w - 2 * kMargin, kTitleH - 10}, 1);
    const auto rects = fileRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect& r = rects[i];
        const FileChoice& f = files_[i];
        hRule(cr, r.y, r.x, r.x + r.w);
        const int lastW = f.lastRead ? measureText("Last read", kLastRead, r.w / 3, 1).width + 10 : 0;
        drawText(cr, f.title, kName, {r.x, r.y + 22, r.w - lastW - (lastW ? 20 : 0), 60}, 1);
        if (f.lastRead) drawText(cr, "Last read", kLastRead, {r.x + r.w - lastW, r.y + 30, lastW, 50}, 1, Align::Right);
        drawText(cr, fileMeta(f), kMeta, {r.x, r.y + 86, r.w, 50}, 1);
    }
}

void FileChoiceScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (!sheetRect().contains(g.pos)) return nav.pop();
    const auto rects = fileRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(g.pos)) continue;
        ItemRow r = row_;
        const AttachmentRecord& a = files_[i].attachment;
        r.attachmentKey = a.key;
        r.badge = a.contentType == "application/pdf" ? "PDF" : "EPUB";
        r.fileState = a.state;
        r.exactFile = true;
        const Choose choose = choose_;  // this screen goes with the pop
        nav.pop();
        if (choose) choose(r, nav);
        return;
    }
}

}  // namespace zk
