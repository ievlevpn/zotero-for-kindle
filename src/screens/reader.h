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
#include "screens/plugin_result.h"
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
    bool onPen(const PenEvent&, Nav&) override;  // strokes it doesn't write with act as a finger
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

    // "Your computer is on page 47. Go there?" when Zotero's position moved elsewhere since this
    // Kindle last agreed with it; Go jumps there, Stay (or reading on) keeps the Kindle's place.
    std::optional<std::string> positionQuestion() const;
    static Rect positionBannerRect();
    static Rect positionGoRect();
    static Rect positionStayRect();

    // Back after a jump (Contents, search, Notes, Go to, the slider, Zotero's position): the
    // footer offers "‹ Back to page 12" until a few pages are read at the new place.
    std::optional<std::string> backLabel() const;
    static Rect backRect();
    // Bookmarks (on this Kindle only): the page's top-right corner sets or removes one.
    static Rect bookmarkRect();
    bool bookmarked() const;
    // The page number a person sees (1-based) and how many: PDF pages, also in column view, whose
    // pages are views of them.
    int shownPage(int page) const;
    int shownPageCount() const;
    int viewsPerPage() const { return views_; }
    void jumpToHit(const SearchHit&);    // in-book search: to the hit's page (its view, in a PDF)
    std::string linkButtonLabel() const;  // the link pop-up's button: Go to note / Go to page N
    std::vector<TocEntry> bookmarks() const;  // by page, for Contents
    void goToPage(int page);                  // Go to… (a jump)
    // Links: a tap on one asks the worker what it is; a note, a PDF reference or a web address
    // opens in a pop-up over the bottom of the page (any tap closes it), a jump goes there.
    const std::vector<LinkBox>& links() const { return links_; }
    const std::optional<LinkInfo>& linkPopup() const { return linkPopup_; }
    Rect linkPopupRect() const;
    Rect linkButtonRect() const;  // "Go to note" / "Go to page N"; empty when there's nothing to go to
    // Turning on past the last page of a book not read yet: a pop-up offering Mark as read. Any
    // gesture closes it; its button marks the item read.
    bool endPrompt() const { return endPrompt_; }
    Rect endPromptRect() const;
    Rect endPromptButtonRect() const;
    bool turning() const { return turning_; }
    bool midTurn() const override { return turning_; }

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
    const PluginResult* pluginResult() const { return result_.get(); }  // a selection action's run

    // Areas (PDF; Zotero `image` annotations): a long press where there's no text and a drag draw
    // one, as do "Mark an area" in the menus (the next drag, over text too) and the pen with its
    // side button held. Saved on lift in the colour used last, then the edit bar opens on it.
    std::optional<Rect> drawnArea() const;  // the rectangle under the finger, while drawn
    bool areaArmed() const { return areaArmed_; }  // "Mark an area": waiting for the drag
    void markArea(Nav&);                           // the menus' "Mark an area" (closes them)
    bool dragsAtOnce() const override { return areaArmed_; }
    bool fastPartsApart() const override { return areaDrawing_; }
    static Rect areaBannerRect();  // "Drag across the area to mark" while armed, at the top of the page
    static Rect areaCancelRect();
    static constexpr int kMinAreaPx = 59;  // ~5 mm at 300 ppi: smaller on either side is a slip
    // A tap selects an area within ~6 mm of its frame (inside or out); one smaller than ~15 mm on
    // a side anywhere inside. Elsewhere inside, the tap is the page's.
    static constexpr int kAreaFramePx = 71, kSmallAreaPx = 177;

    // Handwriting (PDF, Scribe): strokes wait this long after the last one, then become one
    // Zotero ink annotation (a page turn sends them at once).
    static constexpr int64_t kInkCommitMs = 1500;
    static constexpr int kInkWidthPx = 7;  // about the Scribe wet ink line
    // A pen touch within this box and time, with nothing written before it, is a tap.
    static constexpr int kTapSlopPx = 12;
    static constexpr int64_t kTapMs = 300;
    size_t pendingInkStrokes() const { return inkPending_.size(); }
    // The pen on PDF pages: writes (default) or taps like a finger (kept in the library). Holding
    // the pen's side button taps for that stroke either way.
    bool penWrites() const { return penWrites_; }
    void setPenWrites(bool on, Nav&);
    bool penSeen() const { return penSeen_; }  // a pen was used here (the Display panel offers the choice)
    void commitInk();
    // commitInk, then waits (at most `timeoutMs`) until every stroke is an annotation: before
    // the book closes. Back in the menus and onClose call it; after one wait has given up, the
    // next doesn't wait again (the UI would freeze twice for the same strokes).
    void flushInk(Nav&, int64_t timeoutMs = kInkFlushMs);
    static constexpr int64_t kInkFlushMs = 6000;
    // One annotation holds at most this much: steady writing doesn't become one huge annotation
    // that a touch of the eraser deletes whole (and that Zotero might refuse).
    static constexpr size_t kMaxInkStrokes = 24, kMaxInkPoints = 2000;
    static constexpr int64_t kMaxInkSpanMs = 10000;
    void onHide(Nav&) override {
        commitInk();
        dropArea();
    }
    void onBackground(Nav&) override {
        commitInk();
        dropArea();
        if (worker_) worker_->trimCaches();  // MuPDF's store is refilled when we're back
    }
    void onClose(Nav& nav) override {
        if (result_) result_->stop();
        flushInk(nav);
    }

private:
    void startSelection(Point at, Nav&);
    void requestSelection(Point to);
    void onSelection(const RenderEvent&, Nav&);
    void onBarTap(Point, Nav&);
    void showBar(Nav&);
    bool openedSent_ = false;    // bookOpened went to the plugins already
    bool finishedSent_ = false;  // bookFinished went to the plugins already
    void saveSelection(const std::string& type, const std::string& comment, Nav&);
    void closeSelection(Nav&);
    bool editAt(Point, Nav&);  // a highlight under (or right next to) the tap: open its editor
    void closeEditor(Nav&);
    void refreshHighlights(Nav&);
    void sendAnnotations(Nav&, bool onlyIfChanged);
    void turn(int delta, Nav&);
    void openMenus(Nav&);
    bool penAllowed() const;
    void onInk(const RenderEvent&, Nav&);
    void eraseAlong(const std::vector<Point>&, Nav&);
    void beginArea(Point from, Point to, Nav&);
    void moveArea(Point to, Nav&);
    void endArea(Point to, Nav&);  // saves it when big enough
    void cancelArea(Nav&);         // armed or drawn: neither any more
    void dropArea();               // the same without repainting (the page or screen changes anyway)
    void onArea(const RenderEvent&, Nav&);

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
    int views_ = 1;  // PDF column view: views per PDF page (the engine's pages are views)
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
    // A plugin's selection action (More… on the bar): running ("Working…" on the bar), then its pop-up.
    std::unique_ptr<PluginResult> result_;
    void tickResult(Nav&);
    void dropSelection();  // selection and bar gone (no repaint); a first run still going is stopped
    std::optional<AnnotationRecord> openAt_;  // asked for before the book opened
    void markJumpOrigin();  // before a jump: where Back returns to
    void back(Nav&);
    void toggleBookmark(Nav&);
    void sendBookmarks();
    struct BackEntry {
        DocPosition pos;
        int page = 0;
    };
    std::vector<BackEntry> back_;
    bool tapLink(Point, Nav&);
    void followLink(const LinkInfo&);
    std::vector<LinkBox> links_;
    std::optional<LinkInfo> linkPopup_;
    bool endPrompt_ = false;
    uint64_t linkId_ = 0;
    int turnsSinceJump_ = 0;
    static constexpr int kBackTurns = 3;  // pages read at the new place before Back goes
    std::vector<Bookmark> marks_;
    // Where the worker last placed each bookmark, by position (a stale Page event can't pair a
    // bookmark with another's page).
    std::vector<std::pair<std::string, int>> markPlaces_;
    int markPage(const std::string& position) const;  // -1: not placed (yet)
    bool arriving_ = false;  // a jump is landing: if it lands where it started, it needs no Back
    void answerPosition(bool go, Nav&);
    uint64_t placeId_ = 0;  // where Zotero's newer position falls (a Placed event)
    std::string placeAfterFirstPage_;  // Zotero's newer position, asked about once the book shows a page
    // Zotero's position the reader holds (not yet compared): its value, version and page.
    std::string remoteValue_;
    int64_t remoteVersion_ = 0;
    int remotePage_ = -1;
    bool unplaced_ = false;  // Zotero's position can't be placed here: reading on decides
    struct PositionQuestion {
        int page = 0;
        std::string text;
    };
    std::optional<PositionQuestion> question_;
    int twoColumnsSetting_ = -1;  // AppSettings::twoColumns
    // Areas.
    bool areaArmed_ = false, areaDrawing_ = false;
    bool areaPen_ = false;  // drawn with the pen (its side button or Mark an area)
    Point areaFrom_, areaTo_;
    Point selLast_;
    Rect pageBox_;  // where the page is drawn (an area stays on it); empty: the whole page area  // the finger's last place while a long press waits to learn if it is on text
    int64_t areaDownMs_ = 0;
    struct SentArea {
        int page = -1;
        Rect rect;
        std::string colour;  // what it is saved in
    };
    std::map<uint64_t, SentArea> areaSent_;  // being placed by the worker
    struct SavedArea {
        std::string key;
        int page = -1;
        Rect rect;
        std::string colour;
    };
    std::vector<SavedArea> areaSaved_;  // saved; framed until the page comes back with it
    std::string newAreaKey_;                 // just made: its colour, picked on the bar, is the next one's
    // Handwriting.
    bool inkDown_ = false, erasing_ = false;
    bool eraserNear_ = false;  // the system's wet ink pauses: its own eraser would draw
    bool buttonHeld_ = false;  // the side button: the pen taps, no ink
    bool penWrites_ = true, penSeen_ = false;
    bool wantsSystemInk() const override;
    std::string shownAnnotations_;  // sendAnnotations: what the worker was last given
    bool inkFlushGaveUp_ = false;
    std::optional<Point> wipeFrom_;  // a stroke we ignored started here (wet ink may show it)
    std::vector<Point> inkStroke_, eraserPath_;
    std::vector<std::vector<Point>> inkPending_;  // drawn, not yet sent
    struct SentInk {
        int page = -1;
        std::vector<std::vector<Point>> paths;
        bool erased = false;  // a path was erased while it was being placed: place the rest again
    };
    std::map<uint64_t, SentInk> inkSent_;  // sent to the worker, being placed
    struct SavedInk {
        std::string key;  // the annotation it became
        int page = -1;
        std::vector<std::vector<Point>> paths;
    };
    std::vector<SavedInk> inkSaved_;  // saved; drawn (and erasable) until the page comes back with it
    int64_t inkFirstMs_ = 0;          // the first pending stroke's start
    int inkPage_ = -1;
    int64_t inkLastMs_ = 0, inkDownMs_ = 0;
};

}  // namespace zk
