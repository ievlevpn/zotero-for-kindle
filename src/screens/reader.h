#pragma once
// The reader (spec §6.3): the page, and a small footer (chapter + time left; percentage or
// page). Taps on the outer 30% turn pages, swipes too; the middle opens the menus. Pages come
// from a RenderWorker; the UI thread only blits them. A long-press selects text (drag to extend);
// the annotation bar over it saves a highlight, underline or note (Plan 5).

#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "docs/render_worker.h"
#include "screens/screen.h"
#include "ui/text_field.h"

namespace zk {

class ReaderMenus;
class Lipc;
class AnnotationBar;
class AnnotationEditor;

class ReaderScreen final : public Screen {
public:
    using WorkerFactory = std::function<std::unique_ptr<RenderWorker>()>;

    ReaderScreen(ItemRow row, std::string path, DocType type, WorkerFactory makeWorker, int flashEvery,
                 Lipc* lipc = nullptr /* frontlight */);
    ~ReaderScreen() override;

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onPen(const PenEvent&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    // Collects worker events; when not visible (a pushed panel is on top) the page state is kept
    // but nothing is repainted — the reader redraws when it is shown again.
    void pumpEvents(Nav&, bool visible);
    void setSearchListener(std::function<void(uint64_t requestId, const std::vector<SearchHit>&)> f) {
        searchListener_ = std::move(f);
    }
    bool showsTabBar() const override { return false; }
    bool isReading() const override { return true; }
    void onResize(Nav&) override;  // turned: the book is laid out for the new page area
    bool landscape() const { return pageArea().w > pageArea().h; }
    bool twoColumns() const { return layout_.columns == 2; }
    void setTwoColumns(bool on, Nav&);  // the Display panel's choice (kept in the settings)
    bool wantsKeyboard() const override;

    static Rect pageArea();    // the window above the footer (1236 × 1487 on the PW5)
    static Rect footerArea();  // the 60 px below it

    int page() const { return page_; }
    bool ready() const { return image_ != nullptr; }
    const std::vector<HighlightBox>& highlights() const { return highlights_; }
    const std::optional<UserError>& error() const { return error_; }
    std::string footerLeft() const;
    std::string footerRight() const;

    // Used by the menus and panels (Tasks 8–11).
    RenderWorker* worker() { return worker_.get(); }
    const ItemRow& row() const { return row_; }
    DocType type() const { return type_; }
    int pageCount() const { return pageCount_; }
    const std::vector<TocEntry>& toc() const { return toc_; }
    const std::string& chapter() const { return chapter_; }
    const DocPosition& position() const { return position_; }
    Layout layout() const { return layout_; }
    void applyLayout(const Layout&, Nav&);
    void turnTo(int page);
    void jumpTo(const DocPosition&);            // from Contents (menus close)
    void jumpToAnnotation(const AnnotationRecord&);  // from Notes
    void closeMenus(Nav&);
    bool menusOpen() const { return menus_ != nullptr; }
    ReaderMenus* menus() { return menus_.get(); }

    // Selection mode (Plan 5).
    const std::optional<Selection>& selection() const { return selection_; }
    AnnotationBar* annotationBar() { return bar_.get(); }
    bool noteOpen() const { return note_.has_value(); }
    AnnotationEditor* annotationEditor() { return editor_.get(); }
    Rect noteRect() const { return noteBox_; }  // the note box, above the keyboard as it was when opened

    // Handwriting (PDF, Scribe): strokes wait this long after the last one, then become one
    // Zotero ink annotation (a page turn sends them at once).
    static constexpr int64_t kInkCommitMs = 1500;
    static constexpr int kInkWidthPx = 7;  // about the Scribe wet ink line
    size_t pendingInkStrokes() const { return inkPending_.size(); }
    void commitInk();

private:
    void startSelection(Point at, Nav&);
    void requestSelection(Point to);
    void onSelection(const RenderEvent&, Nav&);
    void onBarTap(Point, Nav&);
    void showBar(Nav&);
    void saveSelection(const std::string& type, const std::string& comment, Nav&);
    void closeSelection(Nav&);
    bool editAt(Point, Nav&);  // a highlight under (or right next to) the tap: open its editor
    void closeEditor(Nav&);
    void refreshHighlights(Nav&);
    void turn(int delta, Nav&);
    void openMenus(Nav&);
    bool penAllowed() const;
    void onInk(const RenderEvent&, Nav&);
    void eraseAlong(const std::vector<Point>&, Nav&);

    ItemRow row_;
    std::string path_;
    DocType type_;
    WorkerFactory makeWorker_;
    int flashEvery_;
    Lipc* lipc_;
    std::unique_ptr<RenderWorker> worker_;
    std::shared_ptr<const PageImage> image_;
    std::vector<HighlightBox> highlights_;
    std::vector<TocEntry> toc_;
    std::optional<UserError> error_;
    std::unique_ptr<ReaderMenus> menus_;
    Layout layout_;
    DocPosition position_;
    std::string chapter_;
    int page_ = -1, pageCount_ = 0, turns_ = 0;
    int64_t wordsLeft_ = 0;
    double progress_ = 0;
    bool turning_ = false;
    std::function<void(uint64_t, const std::vector<SearchHit>&)> searchListener_;
    bool layoutPending_ = false;  // re-paginating: page numbers are about to change
    // Selection mode.
    bool selecting_ = false, selDragging_ = false, selReleased_ = false;  // the bar waits for the finger to lift
    Point selAnchor_;
    uint64_t selPending_ = 0;
    std::optional<Selection> selection_;
    std::unique_ptr<AnnotationBar> bar_;
    std::optional<TextField> note_;
    Rect noteBox_;
    std::string colour_ = "#ffd400";  // the colour used last
    std::unique_ptr<AnnotationEditor> editor_;
    std::optional<AnnotationRecord> openAt_;  // asked for before the book opened
    int twoColumnsSetting_ = -1;  // AppSettings::twoColumns
    // Handwriting.
    bool inkDown_ = false, erasing_ = false, inkToastShown_ = false;
    bool eraserNear_ = false;  // the system's wet ink pauses: its own eraser would draw
    std::optional<Point> wipeFrom_;  // a stroke we ignored started here (wet ink may show it)
    std::vector<Point> inkStroke_, eraserPath_;
    std::vector<std::vector<Point>> inkPending_;          // drawn, not yet sent
    std::map<uint64_t, std::vector<std::vector<Point>>> inkSent_;  // sent, being placed
    std::vector<std::vector<Point>> inkSaved_;           // saved; shown until the page comes back with them
    int inkPage_ = -1;
    int64_t inkLastMs_ = 0;
};

}  // namespace zk
