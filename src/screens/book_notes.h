#pragma once
// The highlights and notes of the open book; tapping one goes to its page, a long-press edits it
// (recolour, tags, comment, delete). The header's Tag filters them by a tag (as the Notes tab):
// "tag: method · 12 notes" under the header, with Clear.

#include <functional>
#include <map>
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
    static Rect tagButtonRect();
    static Rect clearRect();
    const std::string& tag() const { return tag_; }
    void setTag(const std::string& tag, Nav&);  // "" none
    std::string filterLine() const;
    const std::vector<AnnotationRecord>& shown() const { return all_; }  // with the filter
    AnnotationEditor* annotationEditor() { return editor_.get(); }

private:
    void reload(Nav&);
    void closeEditor(Nav&);
    Rect contentArea() const;
    ItemRow row_;
    std::function<void(const AnnotationRecord&)> pick_;
    std::vector<AnnotationRecord> all_;
    std::map<std::string, std::string> authors_;  // another member's name, by annotation key
    Pager pager_{0, 1};
    std::string tag_;
    std::unique_ptr<AnnotationEditor> editor_;
};

}  // namespace zk
