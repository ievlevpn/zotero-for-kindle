#pragma once
// The reader (spec §6.3): the page, and a small footer (chapter + time left; percentage or
// page). Taps on the outer 30% turn pages, swipes too; the middle opens the menus. Pages come
// from a RenderWorker; the UI thread only blits them. A long-press selects text (drag to extend);
// the annotation bar over it saves a highlight, underline or note (Plan 5).

#include <functional>
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
    bool wantsKeyboard() const override;

    static Rect pageArea();    // 1236 × 1487 from the top
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
    static Rect noteRect();

private:
    void startSelection(Point at, Nav&);
    void requestSelection(Point to);
    void onSelection(const RenderEvent&, Nav&);
    void onBarTap(Point, Nav&);
    void showBar(Nav&);
    void saveSelection(const std::string& type, const std::string& comment, Nav&);
    void closeSelection(Nav&);
    bool editAt(Point, Nav&);  // a highlight under the tap: open its editor
    void closeEditor(Nav&);
    void refreshHighlights(Nav&);
    void turn(int delta, Nav&);
    void openMenus(Nav&);

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
    std::string colour_ = "#ffd400";  // the colour used last
    std::unique_ptr<AnnotationEditor> editor_;
};

}  // namespace zk
