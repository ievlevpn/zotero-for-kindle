#pragma once
// Go to… (from Contents, and the Library's jump sheet): a number pad for a page number, or a
// percentage ("45%").

#include <functional>
#include <string>

#include "screens/screen.h"

namespace zk {

class GoToPageScreen final : public Screen {
public:
    // `go` gets the page index (0-based) once a valid one is entered; the pad closes itself first.
    // reading: part of the reader (turns to landscape with it); the Library's isn't.
    GoToPageScreen(int pageCount, int currentPage, std::function<void(int page)> go, bool reading = true);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    bool showsTabBar() const override { return false; }
    bool isReading() const override { return reading_; }

    // "0"–"9", "%", "⌫", "Go".
    static Rect keyRect(const std::string& key);
    const std::string& entry() const { return entry_; }
    const std::string& error() const { return error_; }

private:
    void press(const std::string& key, Nav&);
    int pageCount_, current_;
    std::function<void(int)> go_;
    std::string entry_, error_;
    bool reading_;
};

}  // namespace zk
