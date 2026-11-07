#pragma once
// The highlights and notes of the open book; tapping one goes to its page, a long-press edits it
// (recolour, comment, delete).

#include <functional>
#include <memory>

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class AnnotationEditor;

class BookNotesScreen final : public Screen {
public:
    BookNotesScreen(ItemRow row, std::function<void(const AnnotationRecord&)> pick);
    ~BookNotesScreen() override;
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    bool wantsKeyboard() const override;
    bool showsTabBar() const override { return false; }
    bool isReading() const override { return true; }  // turns with the reader
    std::vector<Rect> rowRects() const;
    AnnotationEditor* annotationEditor() { return editor_.get(); }

private:
    void reload(Nav&);
    void closeEditor(Nav&);
    ItemRow row_;
    std::function<void(const AnnotationRecord&)> pick_;
    std::vector<AnnotationRecord> all_;
    Pager pager_{0, 1};
    std::unique_ptr<AnnotationEditor> editor_;
};

}  // namespace zk
