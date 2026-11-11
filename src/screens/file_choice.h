#pragma once
// "Open which file?": tapping an item with more than one readable file (EPUB, PDF) shows this
// sheet over the screen it was tapped on. Each file: its title (else its filename), PDF or EPUB,
// "On Kindle" when downloaded, how far it was read; the one read last first, marked "Last read".
// A tap on a file opens it (the open flow, as a row's tap); one outside the sheet closes it.

#include <functional>
#include <string>
#include <vector>

#include "screens/screen.h"

namespace zk {

class FileChoiceScreen final : public Screen {
public:
    // choose: the item's row for the file tapped (exactFile), after this sheet has closed.
    using Choose = std::function<void(const ItemRow&, Nav&)>;
    // under: the screen it shows over (drawn beneath it); may be null.
    FileChoiceScreen(ItemRow row, std::vector<FileChoice> files, Screen* under, Choose choose);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    bool showsTabBar() const override { return under_ ? under_->showsTabBar() : true; }

    const std::vector<FileChoice>& files() const { return files_; }
    Rect sheetRect() const;
    std::vector<Rect> fileRects() const;  // as many files as fit, in files() order
    static std::string fileMeta(const FileChoice&);  // "EPUB · On Kindle · 45%"

private:
    Rect area() const;  // what the sheet fades: above the tab bar if there is one
    ItemRow row_;
    std::vector<FileChoice> files_;
    Screen* under_;
    Choose choose_;
};

}  // namespace zk
