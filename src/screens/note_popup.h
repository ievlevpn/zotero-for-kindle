#pragma once
// A sticky note's pop-up (Zotero `note` annotations on PDFs), near the note it was opened from:
//
//   Note · p. 12                                              1 / 2
//   The note's text, plain, paged with swipes when it is long.
//   (#method) (#to-cite) (+ Tag)
//   [ Edit ]  [ Delete ]  [ Close ]
//
// Edit, Delete and the tag row (the annotation bar's) are the reader's to do (the annotation
// editor's comment and its confirmation; a chip toggles its tag); any tap outside, or Close, closes it.

#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "screens/annotation_bar.h"
#include "store/records.h"
#include "ui/gesture.h"

namespace zk {

class Nav;

class NotePopup {
public:
    enum class Action { None, Edit, Delete, Close, Tag, AddTag };

    // `icon`: the note as drawn; `area`: where the pop-up may go (the page area); `chips`: the tag row's.
    NotePopup(AnnotationRecord note, Rect icon, Rect area, std::vector<std::string> chips = {});

    const std::string& key() const { return note_.key; }
    const AnnotationRecord& note() const { return note_; }
    Rect icon() const { return icon_; }
    std::string title() const;  // "Note · p. 12"
    Rect rect() const { return rect_; }
    Rect buttonRect(int i) const;  // 0 Edit, 1 Delete, 2 Close
    int page() const { return page_; }
    int pages() const { return static_cast<int>(starts_.size()); }
    const TagRow& tags() const { return tags_; }
    TagRow& tags() { return tags_; }
    int tappedChip() const { return chip_; }  // after Action::Tag
    void setNote(AnnotationRecord a) { note_ = std::move(a); }

    void draw(cairo_t*) const;
    // A button's action; a swipe turns the text's page (None); a tap outside: Close.
    Action onGesture(const Gesture&, Nav&);

private:
    AnnotationRecord note_;
    Rect icon_, area_, rect_;
    std::vector<int> tops_, starts_;  // the text's line tops; the first line of each page
    int page_ = 0;
    TagRow tags_;
    int chip_ = -1;
};

}  // namespace zk
