#include "screens/plugin_screen.h"

#include <algorithm>
#include <functional>

#include "screens/plugin_diff.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;
using json = nlohmann::json;
using K = Block::Kind;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }
const TextStyle kNameText{kSans, 7.5, 500, false, kSecondary};

// The first block (depth first) that `pred` accepts.
const Block* findBlock(const Block& b, const std::function<bool(const Block&)>& pred) {
    if (pred(b)) return &b;
    if (b.kind == K::Column || b.kind == K::Row)
        for (const auto& c : b.children)
            if (const Block* f = findBlock(c, pred)) return f;
    return nullptr;
}

const Block* input(const Block& root, const std::string& id) {
    return findBlock(root, [&](const Block& b) { return (b.kind == K::Search || b.kind == K::Field) && b.id == id; });
}

void eachInput(const Block& b, const std::function<void(const Block&)>& f) {
    if (b.kind == K::Search || b.kind == K::Field) f(b);
    if (b.kind == K::Column || b.kind == K::Row)
        for (const auto& c : b.children) eachInput(c, f);
}

// Did a deck's card change (a new card starts at its first page)?
bool cardChanged(const Block& before, const Block& after) {
    bool changed = false;
    std::function<void(const Block&)> walk = [&](const Block& b) {
        if (b.kind == K::Deck) {
            const Block* old = findBlock(before, [&](const Block& o) { return o.kind == K::Deck && o.id == b.id; });
            if (!old || old->children != b.children) changed = true;
        }
        if (b.kind == K::Column || b.kind == K::Row)
            for (const auto& c : b.children) walk(c);
    };
    walk(after);
    return changed;
}

void popCodePoint(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

}  // namespace

PluginScreen::PluginScreen(ScreenLink& link, uint64_t instance, std::string pluginName, std::string title)
    : link_(link), instance_(instance), pluginName_(std::move(pluginName)), title_(std::move(title)) {}

Rect PluginScreen::area(Nav& nav) const {
    Rect a = blocksArea();
    const Rect kb = nav.keyboard().bounds();
    if (!kb.empty() && kb.y < a.y + a.h) a.h = std::max(0, kb.y - 12 - a.y);
    return a;
}

Rect PluginScreen::workingRect() { return {(W() - 420) / 2, kHeaderH + 16, 420, 96}; }

Rect PluginScreen::backButtonRect() { return {(W() - 400) / 2, kHeaderH + 360, 400, 110}; }

int PluginScreen::nameReserve() const {
    return std::min(measureText(pluginName_, kNameText, W() / 3, 1).width + 40, W() / 3);
}

void PluginScreen::relayout(Nav& nav) {
    if (!tree_) return;
    const Rect a = area(nav);
    laid_ = layoutTree(*tree_, a, page_);
    page_ = laid_.page;
    if (focus_.focused.empty() || findLaid(laid_, focus_.focused)) return;
    for (int p = 0; p < laid_.pages; ++p) {  // the page that holds the focused input
        Laid l = layoutTree(*tree_, a, p);
        if (findLaid(l, focus_.focused)) {
            laid_ = std::move(l);
            page_ = p;
            return;
        }
    }
}

void PluginScreen::onShow(Nav& nav) { relayout(nav); }

void PluginScreen::setFocus(const std::string& id, Nav& nav) {
    if (id == focus_.focused) return;
    flushChange();
    focus_.focused = id;
    if (id.empty()) nav.keyboard().hide();
    else nav.keyboard().show();
    relayout(nav);
    nav.redrawAll(Waveform::Quality);
}

void PluginScreen::receive(const PluginHostEvent& ev, Nav& nav) {
    switch (ev.kind) {
        case PluginHostEvent::Kind::ScreenTree:
            if (state_ != State::Failed && ev.tree) showTree(ev.tree, ev.requestId, nav);
            break;
        case PluginHostEvent::Kind::ScreenError:
            state_ = State::Failed;
            error_ = ev.text;
            queue_.clear();
            pendingChange_.clear();
            working_ = false;
            if (!focus_.focused.empty()) nav.keyboard().hide();
            focus_ = {};
            nav.redrawAll(Waveform::Quality);
            break;
        case PluginHostEvent::Kind::ScreenActionError:
            nav.toast(ev.text);
            break;
        case PluginHostEvent::Kind::ScreenClose:
            closedByPlugin_ = true;
            if (!focus_.focused.empty()) nav.keyboard().hide();
            nav.pop();
            break;
        default:
            break;
    }
}

void PluginScreen::showTree(std::shared_ptr<const Block> tree, uint64_t requestId, Nav& nav) {
    acked_ = std::max(acked_, requestId);
    const std::shared_ptr<const Block> old = tree_;  // the old Laid points into it
    const Laid before = std::move(laid_);
    const bool first = state_ != State::Shown;
    tree_ = std::move(tree);
    state_ = State::Shown;
    // `value`: shown whenever it differs from the text last sent (or last shown from the plugin).
    eachInput(*tree_, [&](const Block& b) {
        if (!b.value) return;
        auto it = lastSent_.find(b.id);
        if (it != lastSent_.end() && it->second == *b.value) return;
        focus_.texts[b.id] = *b.value;
        lastSent_[b.id] = *b.value;
        if (pendingChange_ == b.id) pendingChange_.clear();
    });
    if (!sending() && queue_.empty()) focus_.chips.clear();  // the plugin's selection stands
    if (old && cardChanged(*old, *tree_)) page_ = 0;
    std::string focused = focus_.focused;
    if (!focused.empty() && !input(*tree_, focused)) focused.clear();
    if (first && focused.empty())
        if (const Block* f = findBlock(*tree_, [](const Block& b) { return (b.kind == K::Search || b.kind == K::Field) && b.focus; }))
            focused = f->id;
    if (focused != focus_.focused) {
        setFocus(focused, nav);  // lays out and redraws all
        return;
    }
    relayout(nav);
    if (first || !old) {
        nav.redrawAll(Waveform::Quality);
        return;
    }
    for (const Damage& d : diffTrees(before, laid_)) nav.damage(d.rect, d.wave);
}

void PluginScreen::flushChange() {
    if (pendingChange_.empty()) return;
    const std::string id = pendingChange_;
    pendingChange_.clear();
    const std::string text = focus_.texts[id];
    lastSent_[id] = text;
    for (auto& q : queue_)  // one not sent yet: it carries the latest text
        if (q["kind"] == "change" && q["id"] == id) {
            q["text"] = text;
            return;
        }
    queue_.push_back({{"kind", "change"}, {"id", id}, {"text", text}});
}

void PluginScreen::enqueue(json event, Nav& nav) {
    if (state_ != State::Shown) return;
    flushChange();  // a pending change goes first
    if (event["kind"] == "swipe")
        for (const auto& q : queue_)
            if (q["kind"] == "swipe" && q["id"] == event["id"] && q["direction"] == event["direction"]) return;
    queue_.push_back(std::move(event));
    pump(nav);
}

void PluginScreen::pump(Nav& nav) {
    if (state_ != State::Shown || sending() || queue_.empty()) return;
    link_.send(instance_, std::move(queue_.front()));
    queue_.pop_front();
    ++sent_;
    sentAtMs_ = nav.nowMs();
}

void PluginScreen::onTick(Nav& nav) {
    PluginHostEvent ev;
    while (!closedByPlugin_ && link_.take(instance_, ev)) receive(ev, nav);
    if (closedByPlugin_) return;
    if (!pendingChange_.empty() && nav.nowMs() - lastKeyMs_ >= kPauseMs) flushChange();
    pump(nav);
    const bool w = state_ == State::Shown && sending() && nav.nowMs() - sentAtMs_ >= kWorkingMs;
    if (w != working_) {
        working_ = w;
        nav.damage(workingRect(), Waveform::Ui);
    }
}

void PluginScreen::leave(Nav& nav) {
    if (!focus_.focused.empty()) nav.keyboard().hide();
    nav.pop();
}

void PluginScreen::onClose(Nav&) {
    if (closedByPlugin_ || left_) return;
    left_ = true;
    flushChange();  // what was typed reaches the plugin; other events not sent yet are dropped
    for (auto& q : queue_)
        if (q["kind"] == "change") link_.send(instance_, std::move(q));
    queue_.clear();
    link_.left(instance_);
}

void PluginScreen::turnPage(int delta, Nav& nav) {
    if (!tree_ || laid_.pages <= 1) return;
    const int np = std::clamp(page_ + delta, 0, laid_.pages - 1);
    if (np == page_) return;
    const Laid before = std::move(laid_);
    page_ = np;
    laid_ = layoutTree(*tree_, area(nav), page_);
    if (!focus_.focused.empty() && !findLaid(laid_, focus_.focused)) {
        setFocus("", nav);  // its input went off the page: the keyboard goes
        return;
    }
    for (const Damage& d : diffTrees(before, laid_)) nav.damage(d.rect, d.wave);
}

void PluginScreen::tapBlocks(Point p, Nav& nav) {
    const auto hit = hitTest(laid_, p);
    if (!hit) return;
    switch (hit->kind) {
        case K::Button:
        case K::Buttons: enqueue({{"kind", "tap"}, {"id", hit->id}}, nav); break;
        case K::List: enqueue({{"kind", "select"}, {"id", hit->id}, {"row", hit->part}}, nav); break;
        case K::Chips: {
            const Laid* l = findLaid(laid_, hit->id);
            if (!l) return;
            const Block& b = *l->block;
            auto shown = focus_.chips.find(hit->id);
            std::vector<std::string> sel = shown != focus_.chips.end() ? shown->second : b.selected;
            const bool was = std::find(sel.begin(), sel.end(), hit->part) != sel.end();
            if (b.select == Block::Select::None) sel.clear();
            else if (b.select == Block::Select::One) sel = was ? std::vector<std::string>{} : std::vector<std::string>{hit->part};
            else if (was) sel.erase(std::remove(sel.begin(), sel.end(), hit->part), sel.end());
            else sel.push_back(hit->part);
            if (b.select != Block::Select::None) {  // shown at once; the plugin's next render decides
                focus_.chips[hit->id] = sel;
                nav.damage(l->rect, Waveform::Ui);
            }
            enqueue({{"kind", "chips"}, {"id", hit->id}, {"chip", hit->part}, {"selected", sel}}, nav);
            break;
        }
        case K::Search:
        case K::Field: setFocus(hit->id, nav); break;
        default: break;
    }
}

void PluginScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos)) {
        leave(nav);
        return;
    }
    if (state_ == State::Failed) {
        if (g.type == GestureType::Tap && backButtonRect().contains(g.pos)) leave(nav);
        return;
    }
    if (state_ != State::Shown) return;
    if (g.type == GestureType::Tap) {
        tapBlocks(g.pos, nav);
    } else if (g.type == GestureType::Swipe) {
        const auto hit = hitTest(laid_, g.start);
        const bool sideways = g.dir == Direction::Left || g.dir == Direction::Right;
        if (hit && hit->kind == K::Deck && sideways)
            enqueue({{"kind", "swipe"}, {"id", hit->id}, {"direction", g.dir == Direction::Left ? "left" : "right"}}, nav);
        else if (g.dir == Direction::Up || g.dir == Direction::Left) turnPage(1, nav);
        else if (g.dir == Direction::Down || g.dir == Direction::Right) turnPage(-1, nav);
    }
}

void PluginScreen::onKey(const KeyEvent& k, Nav& nav) {
    if (state_ != State::Shown || focus_.focused.empty() || !tree_) return;
    const std::string id = focus_.focused;
    const Block* b = input(*tree_, id);
    if (!b) return;
    if (k.code == KeyEvent::Code::Escape) {
        setFocus("", nav);
        return;
    }
    if (!focus_.texts.count(id)) focus_.texts[id] = b->value.value_or("");
    std::string& t = focus_.texts[id];
    const Laid* l = findLaid(laid_, id);
    if (k.code == KeyEvent::Code::Text) t += k.text;
    else if (k.code == KeyEvent::Code::Backspace) popCodePoint(t);
    else if (k.code == KeyEvent::Code::Return) {
        if (b->kind == K::Field && b->lines > 1) t += "\n";
        else {
            pendingChange_ = id;
            const std::string text = t;
            enqueue({{"kind", "submit"}, {"id", id}, {"text", text}}, nav);  // after its change
            lastSent_[id] = text;
            return;
        }
    }
    pendingChange_ = id;
    lastKeyMs_ = nav.nowMs();
    if (l) nav.damage(l->rect, Waveform::Fast);
}

void PluginScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    const int reserve = nameReserve();
    drawHeader(cr, title_, true, reserve);
    centredText(cr, pluginName_, kNameText, {W() - kMargin - reserve + 40, 30, reserve - 40, 92}, Align::Right);
    switch (state_) {
        case State::Loading:
            centredText(cr, "Loading…", {kSans, 9, 400, false, kSecondary}, {kMargin, kHeaderH + 200, W() - 2 * kMargin, 100});
            break;
        case State::Failed:
            drawText(cr, error_, {kSerif, 11, 400, false, kSecondary}, {kMargin, kHeaderH + 160, W() - 2 * kMargin, 180}, 3,
                     Align::Center);
            drawButton(cr, backButtonRect(), "Back", false);
            break;
        case State::Shown:
            drawTree(cr, laid_, focus_);
            if (working_) drawToast(cr, workingRect(), "Working…");
            break;
    }
}

}  // namespace zk
