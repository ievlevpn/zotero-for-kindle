#pragma once
// Go to… (from Contents): a number pad for a page number, or a percentage ("45%").

#include <functional>
#include <string>

#include "screens/screen.h"

namespace zk {

class GoToPageScreen final : public Screen {
public:
    // `go` gets the page index (0-based) once a valid one is entered; the pad closes itself first.
    GoToPageScreen(int pageCount, int currentPage, std::function<void(int page)> go);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    bool showsTabBar() const override { return false; }
    bool isReading() const override { return true; }

    // "0"–"9", "%", "⌫", "Go".
    static Rect keyRect(const std::string& key);
    const std::string& entry() const { return entry_; }
    const std::string& error() const { return error_; }

private:
    void press(const std::string& key, Nav&);
    int pageCount_, current_;
    std::function<void(int)> go_;
    std::string entry_, error_;
};

}  // namespace zk
