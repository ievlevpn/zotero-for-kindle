#include "platform/keyboard.h"

#include "core/log.h"

namespace zk {

namespace {

class KindleKeyboard final : public Keyboard {
public:
    explicit KindleKeyboard(Lipc& lipc) : lipc_(lipc) {}
    ~KindleKeyboard() override { hide(); }

    void show() override {
        if (shown_) return;
        // The argument names our window's ID from its title (S3).
        lipc_.set("com.lab126.keyboard", "open", "org.zotero.kindle:abc:0");
        // Our window sits under the status bar: its measured top, not the PW5's 101.
        bounds_ = parseKeyboardBounds(lipc_.get("com.lab126.keyboard", "bounds"), metrics().origin.y);
        if (bounds_.w == 0) bounds_ = keyboardFallback(metrics(), profileFor(metrics().device, metrics().ppi));
        shown_ = true;
    }
    void hide() override {
        if (!shown_) return;
        lipc_.set("com.lab126.keyboard", "close", "org.zotero.kindle");  // an empty argument is ignored (S3)
        shown_ = false;
    }
    Rect bounds() const override { return shown_ ? bounds_ : Rect{}; }

private:
    Lipc& lipc_;
    bool shown_ = false;
    Rect bounds_{};
};

}  // namespace

std::unique_ptr<Keyboard> makeKeyboard(Lipc& lipc) { return std::make_unique<KindleKeyboard>(lipc); }

}  // namespace zk
