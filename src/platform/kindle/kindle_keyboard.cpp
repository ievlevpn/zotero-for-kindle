#include "platform/keyboard.h"

#include "core/log.h"

namespace zk {

namespace {

constexpr int kWindowTop = 101;  // our window sits under the status bar (S5)

class KindleKeyboard final : public Keyboard {
public:
    explicit KindleKeyboard(Lipc& lipc) : lipc_(lipc) {}
    ~KindleKeyboard() override { hide(); }

    void show() override {
        if (shown_) return;
        // The argument names our window's ID from its title (S3).
        lipc_.set("com.lab126.keyboard", "open", "org.zotero.kindle:abc:0");
        bounds_ = parseKeyboardBounds(lipc_.get("com.lab126.keyboard", "bounds"), kWindowTop);
        if (bounds_.w == 0) bounds_ = {0, 1547 - 567, 1236, 567};  // S3's measured size
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
