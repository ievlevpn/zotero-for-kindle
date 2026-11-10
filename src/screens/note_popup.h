#pragma once
// A sticky note's pop-up (Zotero `note` annotations on PDFs), near the note it was opened from:
//
//   Note · p. 12                                              1 / 2
//   The note's text, plain, paged with swipes when it is long.
//   [ Edit ]  [ Delete ]  [ Close ]
//
// Edit and Delete are the reader's to do (the annotation editor's comment and its confirmation);
// any tap outside, or Close, closes it.

#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "store/records.h"
#include "ui/gesture.h"

namespace zk {

class Nav;

class NotePopup {
public:
    enum class Action { None, Edit, Delete, Close };

    // `icon`: the note as drawn; `area`: where the pop-up may go (the page area).
    NotePopup(AnnotationRecord note, Rect icon, Rect area);

    const std::string& key() const { return note_.key; }
    const AnnotationRecord& note() const { return note_; }
    Rect icon() const { return icon_; }
    std::string title() const;  // "Note · p. 12"
    Rect rect() const { return rect_; }
    Rect buttonRect(int i) const;  // 0 Edit, 1 Delete, 2 Close
    int page() const { return page_; }
    int pages() const { return static_cast<int>(starts_.size()); }

    void draw(cairo_t*) const;
    // A button's action; a swipe turns the text's page (None); a tap outside: Close.
    Action onGesture(const Gesture&, Nav&);

private:
    AnnotationRecord note_;
    Rect icon_, area_, rect_;
    std::vector<int> tops_, starts_;  // the text's line tops; the first line of each page
    int page_ = 0;
};

}  // namespace zk
