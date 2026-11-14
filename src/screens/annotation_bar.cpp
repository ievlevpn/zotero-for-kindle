#include "screens/annotation_bar.h"

#include "platform/device_profile.h"
#include "ui/metrics.h"

#include <algorithm>
#include <set>

#include "docs/document.h"
#include "screens/read_tag.h"
#include "screens/reader.h"
#include "screens/screen.h"
#include "ui/text.h"
#include "ui/text_field.h"
#include "ui/paint.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kSide = 40, kGap = 24, kPad = 20, kDotRowH = 130, kButtonH = 84, kTagGap = 12;
constexpr int kChipGap = 16, kChipPad = 30, kAddW = 190;
const TextStyle kChipText{kSans, 7.5, 500};

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

// ---- the tag row ------------------------------------------------------------------------------

TagRow::TagRow(std::vector<std::string> chips, std::vector<std::string> on) : on_(std::move(on)) { setChips(std::move(chips)); }

void TagRow::setChips(std::vector<std::string> chips) {
    chips_ = std::move(chips);
    if (chips_.size() > kChips) chips_.resize(kChips);
    if (!rect_.empty()) place(rect_);
}

bool TagRow::isOn(const std::string& tag) const { return has(on_, tag); }

void TagRow::place(Rect row) {
    rect_ = row;
    chipRects_.clear();
    const int n = static_cast<int>(chips_.size());
    const int addW = std::min(kAddW, row.w);
    // + Tag on the right; the chips from the left, each up to an equal share of what is left.
    addRect_ = {row.x + row.w - addW, row.y, addW, row.h};
    if (n == 0) {
        addRect_.x = row.x;
        return;
    }
    const int room = row.w - addW - kChipGap;
    const int share = (room - (n - 1) * kChipGap) / n;
    int x = row.x;
    for (const auto& c : chips_) {
        const int w = std::clamp(measureText("#" + c, kChipText, 4000, 1).width + 2 * kChipPad, std::min(150, share), share);
        chipRects_.push_back({x, row.y, w, row.h});
        x += w + kChipGap;
    }
    addRect_.x = x;  // right after the chips
}

void TagRow::draw(cairo_t* cr) const {
    auto chip = [&](Rect r, const std::string& label, bool filled, int weight) {
        roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, r.h / 2.0);
        gray(cr, kInk);
        if (filled) {
            cairo_fill(cr);
        } else {
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
        }
        TextStyle st = kChipText;
        st.weight = weight;
        st.gray = filled ? 255 : kInk;
        centredText(cr, label, st, {r.x + kChipPad / 2, r.y, r.w - kChipPad, r.h});
    };
    for (size_t i = 0; i < chipRects_.size(); ++i) chip(chipRects_[i], "#" + chips_[i], isOn(chips_[i]), 500);
    chip(addRect_, "+ Tag", false, 700);
}

int TagRow::hit(Point p) const {
    for (size_t i = 0; i < chipRects_.size(); ++i)
        if (chipRects_[i].contains(p)) return static_cast<int>(i);
    return addRect_.contains(p) ? kAdd : -1;
}

std::vector<std::string> recentChips(Library& lib) {
    std::vector<std::string> out = lib.recentAnnotationTags();
    if (out.size() > TagRow::kChips) out.resize(TagRow::kChips);
    return out;
}

std::optional<std::vector<std::string>> setAnnotationTag(Nav& nav, const std::string& key, const std::string& tag, bool on) {
    auto now = nav.library().annotation(key);
    if (!now || now->deletedHere) return std::nullopt;
    AnnotationRecord a = *now;  // only the tags change, on the annotation as it is now
    if (on) nav.library().rememberAnnotationTag(tag);
    if (on == has(a.tags, tag)) return a.tags;
    if (on) a.tags.push_back(tag);
    else a.tags.erase(std::remove(a.tags.begin(), a.tags.end(), tag), a.tags.end());
    nav.library().editAnnotation(a);
    plugins::annotationEvent(nav.plugins(), nav.library(), "highlightChanged", a.key);
    return a.tags;
}

void pickAnnotationTag(Nav& nav, std::function<void(const std::string&)> chosen, bool reading) {
    TagPickerScreen::Options o;
    o.source = [](Library& lib) {
        // The recent ones first, then those on annotations (most used), then the items' tags.
        TagPickerScreen::Tags out;
        std::set<std::string> seen;
        const auto used = lib.annotationTags();
        auto countOf = [&](const std::string& t) -> int64_t {
            for (const auto& u : used)
                if (u.first == t) return u.second;
            return 0;
        };
        for (const auto& t : lib.recentAnnotationTags())
            if (seen.insert(t).second) out.emplace_back(t, countOf(t));
        for (const auto& u : used)
            if (seen.insert(u.first).second) out.push_back(u);
        for (const auto& t : lib.topTags(1 << 20))
            if (seen.insert(t.first).second) out.emplace_back(t.first, 0);
        return out;
    };
    o.placeholder = "Search or add a tag";
    o.note = "Put a tag on this highlight. Tags you use here come first.";
    o.unit = "note";
    o.addNew = true;
    o.reading = reading;
    nav.push(std::make_unique<TagPickerScreen>(std::move(chosen), std::move(o)));
}

// ---- the bar ----------------------------------------------------------------------------------

const std::array<ZoteroColour, 8>& zoteroColours() {
    static const std::array<ZoteroColour, 8> colours = {{{"#ffd400", "Yellow"},
                                                         {"#ff6666", "Red"},
                                                         {"#5fb236", "Green"},
                                                         {"#2ea8e5", "Blue"},
                                                         {"#a28ae5", "Purple"},
                                                         {"#e56eee", "Magenta"},
                                                         {"#f19837", "Orange"},
                                                         {"#aaaaaa", "Gray"}}};
    return colours;
}

std::string annotationAuthor(Library& lib, const AnnotationRecord& a) {
    if (a.lib == kMyLibrary || a.createdHere || a.authorName.empty()) return "";
    if (a.authorId != 0 && a.authorId == lib.userId()) return "";
    return a.authorName;
}

std::string annotationReadOnly(Library& lib, const AnnotationRecord& a) {
    if (lib.annotationEditable(a)) return "";
    const std::string author = annotationAuthor(lib, a);
    return author.empty() ? kReadOnlyGroup : "by " + author;
}

namespace {
const TextStyle kViewComment{kSerif, 8.5, 400};
}  // namespace

void AnnotationBar::setView(std::string label, std::string comment) {
    label_ = std::move(label);
    comment_ = std::move(comment);
    const int w = ReaderScreen::pageArea().w - 2 * kSide - 80;
    commentH_ = comment_.empty() ? 0 : measureText(comment_, kViewComment, w, 2).height;
    place(height());
}

AnnotationBar::AnnotationBar(Mode mode, Rect anchor, std::string colour, bool more, TagRow tags)
    : mode_(mode), anchor_(anchor), colour_(std::move(colour)), more_(more && (mode == Mode::Create || mode == Mode::Text)), tags_(std::move(tags)) {
    place(height());
}

void AnnotationBar::toggleTag(const std::string& tag) {
    std::vector<std::string> on = tags_.on();
    if (has(on, tag)) on.erase(std::remove(on.begin(), on.end(), tag), on.end());
    else on.push_back(tag);
    tags_.setOn(std::move(on));
}

void AnnotationBar::place(int h) {
    const Rect page = ReaderScreen::pageArea();
    const int w = page.w - 2 * kSide;
    // Above the selection, else below it; if neither has room (a tall selection or area), at the
    // page's edge on the side with more room, covering as little of it as can be.
    const int above = anchor_.y - kGap - (page.y + kGap), below = page.y + page.h - kGap - (anchor_.y + anchor_.h + kGap);
    int y = anchor_.y - h - kGap;
    if (h > above) y = h <= below || below > above ? anchor_.y + anchor_.h + kGap : page.y + kGap;
    if (y + h > page.y + page.h - kGap) y = page.y + page.h - kGap - h;  // never off the page
    if (y < page.y + kGap) y = page.y + kGap;
    rect_ = {page.x + kSide, y, w, h};
    if (hasColours()) tags_.place({rect_.x + kPad, rect_.y + kPad + kDotRowH + 6, rect_.w - 2 * kPad, TagRow::kH});
}

constexpr int kActionRowH = 96;

void AnnotationBar::showActions(std::vector<PluginAction> actions) {
    actions_ = std::move(actions);
    const int fit = (ReaderScreen::pageArea().h - 2 * kGap - 2 * kPad) / kActionRowH;
    if (static_cast<int>(actions_.size()) > fit) actions_.resize(static_cast<size_t>(std::max(fit, 0)));
    place(actions_.empty() ? height() : 2 * kPad + static_cast<int>(actions_.size()) * kActionRowH);
}

Rect AnnotationBar::actionRect(int i) const {
    return {rect_.x + kPad, rect_.y + kPad + i * kActionRowH, rect_.w - 2 * kPad, kActionRowH};
}

Rect AnnotationBar::dotRect(int i) const {
    if (!hasColours()) return {};
    const int w = (rect_.w - 2 * kPad) / 8;
    return {rect_.x + kPad + i * w, rect_.y + kPad, w, kDotRowH};
}

Rect AnnotationBar::buttonRect(int i) const {
    const int n = buttons();
    const int w = (rect_.w - 2 * kPad - (n - 1) * kGap) / n;
    return {rect_.x + kPad + i * (w + kGap), rect_.y + kPad + (hasColours() ? kDotRowH + 6 + TagRow::kH + kTagGap : 0), w,
            kButtonH};
}

void AnnotationBar::draw(cairo_t* cr) const {
    roundedRect(cr, rect_.x + 1.5, rect_.y + 1.5, rect_.w - 3, rect_.h - 3, 18);
    gray(cr, 255);
    cairo_fill_preserve(cr);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    if (listing()) {  // More…: the plugins' actions, the plugin's name dimmed beside each
        const TextStyle title{kSans, 8, 500}, name{kSans, 7, 400, false, kSecondary};
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
            const Rect r = actionRect(i);
            const int nameW = std::min(measureText(actions_[i].pluginName, name, r.w / 2, 1).width + 10, r.w / 2);
            centredText(cr, actions_[i].title, title, {r.x + 20, r.y, r.w - nameW - 60, r.h}, Align::Left);
            centredText(cr, actions_[i].pluginName, name, {r.x + r.w - nameW - 20, r.y, nameW, r.h}, Align::Right);
            if (i + 1 < static_cast<int>(actions_.size())) hRule(cr, r.y + r.h - 1, r.x, r.x + r.w);
        }
        return;
    }
    if (mode_ == Mode::View) {  // "by Anna", and the comment: nothing here can be changed
        const int labelH = comment_.empty() ? kTextH : kViewLabelH;
        centredText(cr, label_, {kSans, 8, 500, false, kSecondary}, {rect_.x + 40, rect_.y + (comment_.empty() ? 0 : 10), rect_.w - 80, labelH},
                    Align::Left);
        if (!comment_.empty()) drawText(cr, comment_, kViewComment, {rect_.x + 40, rect_.y + kViewLabelH, rect_.w - 80, commentH_}, 2);
        return;
    }
    for (int i = 0; i < (hasColours() ? 8 : 0); ++i) {
        const Rect d = dotRect(i);
        const auto& c = zoteroColours()[i];
        const double cx = d.x + d.w / 2.0, cy = d.y + 38;
        gray(cr, shadeOf(c.hex));
        cairo_arc(cr, cx, cy, 30, 0, 2 * 3.14159265);
        cairo_fill_preserve(cr);
        gray(cr, 120);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
        if (colour_ == c.hex) {  // the chosen colour: a ring around its dot
            gray(cr, kInk);
            cairo_set_line_width(cr, 5);
            cairo_arc(cr, cx, cy, 40, 0, 2 * 3.14159265);
            cairo_stroke(cr);
        }
        centredText(cr, c.name, {kSans, 6, colour_ == c.hex ? 700 : 400, false, kSecondary}, {d.x, d.y + 84, d.w, 40});
    }
    if (hasColours()) tags_.draw(cr);  // Text: no tag row
    static const char* create[] = {"Highlight", "Underline", "Note", "More…"};
    static const char* edit[] = {"Comment", "Delete"};
    static const char* text[] = {"Search", "More…"};
    for (int i = 0; i < buttons(); ++i) {
        const char* label = mode_ == Mode::Create ? create[i] : mode_ == Mode::Edit ? edit[i] : text[i];
        const bool more = mode_ != Mode::Edit && i == buttons() - 1 && more_;
        drawButton(cr, buttonRect(i), more && working_ ? "Working…" : label, false);
    }
}

AnnotationBar::Hit AnnotationBar::hit(Point p) const {
    if (listing()) {
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i)
            if (actionRect(i).contains(p)) return {Action::Plugin, i};
        return {};
    }
    for (int i = 0; i < (hasColours() ? 8 : 0); ++i)
        if (dotRect(i).contains(p)) return {Action::Colour, i};
    if (const int t = hasColours() ? tags_.hit(p) : -1; t == TagRow::kAdd) return {Action::AddTag, -1};
    else if (t >= 0) return {Action::Tag, t};
    static const Action create[] = {Action::Highlight, Action::Underline, Action::Note, Action::More};
    static const Action edit[] = {Action::Comment, Action::Delete};
    static const Action text[] = {Action::Search, Action::More};
    for (int i = 0; i < buttons(); ++i)
        if (buttonRect(i).contains(p))
            return {mode_ == Mode::Create ? create[i] : mode_ == Mode::Edit ? edit[i] : text[i], -1};
    return {};
}

AnnotationEditor::AnnotationEditor(AnnotationRecord a, Rect anchor, std::vector<std::string> chips, std::string readOnly)
    : annotation_(std::move(a)),
      bar_(readOnly.empty() ? AnnotationBar::Mode::Edit : AnnotationBar::Mode::View, anchor, annotation_.color, false,
           readOnly.empty() ? TagRow(std::move(chips), annotation_.tags) : TagRow()),
      self_(std::make_shared<AnnotationEditor*>(this)) {
    if (!readOnly.empty()) bar_.setView(std::move(readOnly), annotation_.type == "note" ? "" : annotation_.comment);
}

AnnotationEditor::~AnnotationEditor() { *self_ = nullptr; }

// Just above the device's keyboard (on the PW5: {40, 690, 1156, 260}).
Rect AnnotationEditor::commentRect() {
    const int keyboard = profileFor(metrics().device, 300).keyboardHeightPx;
    const int top = keyboard > 0 ? metrics().height - keyboard : metrics().height / 2;
    return {40, top - 290, metrics().width - 80, 260};
}

Rect AnnotationEditor::commentRectAbove(Rect keyboard) {
    if (keyboard.empty()) return commentRect();
    return {40, std::max(0, keyboard.y - 290), metrics().width - 80, 260};
}

Rect AnnotationEditor::rect() const { return state_ == State::Comment ? commentBox_ : bar_.rect(); }

Rect AnnotationEditor::confirmRect(int i) const { return bar_.buttonRect(i); }

std::string AnnotationEditor::question() const {
    if (annotation_.type == "note") return "Delete this note?";
    return annotation_.type == "image" ? "Delete this area?" : "Delete this highlight?";
}

void AnnotationEditor::editComment(Nav& nav) {
    if (readOnly()) return;  // the bar says why
    direct_ = true;
    enter(State::Comment, nav);
}

void AnnotationEditor::confirmDelete(Nav& nav) {
    if (readOnly()) return;
    direct_ = true;
    enter(State::Confirm, nav);
}

bool AnnotationEditor::takeChanged() {
    const bool c = changed_;
    changed_ = false;
    return c;
}

void AnnotationEditor::enter(State s, Nav& nav) {
    nav.damage(rect(), Waveform::Ui);
    if (s == State::Comment) {
        comment_ = annotation_.comment;
        nav.keyboard().show();
        commentBox_ = commentRectAbove(nav.keyboard().bounds());
    } else if (state_ == State::Comment) {
        nav.keyboard().hide();
    }
    state_ = s;
    nav.damage(rect(), Waveform::Ui);
}

void AnnotationEditor::draw(cairo_t* cr) const {
    if (state_ == State::Bar) return bar_.draw(cr);
    const Rect r = rect();
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 18);
    gray(cr, 255);
    cairo_fill_preserve(cr);
    gray(cr, kInk);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    if (state_ == State::Confirm) {
        centredText(cr, question(), {kSans, 9, 500}, {r.x + kPad, r.y + kPad, r.w - 2 * kPad, kDotRowH});
        drawButton(cr, confirmRect(0), "Delete", true);
        drawButton(cr, confirmRect(1), "Cancel", false);
        return;
    }
    drawText(cr, annotation_.type == "note" ? "Note" : "Comment", {kSans, 8, 500}, {r.x + 30, r.y + 24, r.w - 60, 50}, 1);
    const bool empty = comment_.empty();
    drawText(cr, empty ? "Type a comment…" : comment_, {kSerif, 9, 400, false, empty ? kTertiary : kInk},
             {r.x + 30, r.y + 84, r.w - 60, 110}, 2);
    drawText(cr, "Return saves", {kSans, 6, 400, false, kTertiary}, {r.x + 30, r.y + r.h - 56, r.w - 60, 40}, 1, Align::Right);
}

AnnotationEditor::Result AnnotationEditor::onTap(Point p, Nav& nav) {
    switch (state_) {
        case State::Comment:
            if (commentBox_.contains(p)) return Result::Open;
            nav.keyboard().hide();
            return Result::Closed;
        case State::Confirm:
            if (confirmRect(0).contains(p)) {
                const bool told = nav.plugins().listening();
                const plugins::AnnotationRef ref = told ? plugins::annotationRef(nav.library(), annotation_) : plugins::AnnotationRef{};
                nav.library().deleteAnnotation(annotation_.key);
                if (told) plugins::annotationDeleted(nav.plugins(), ref);
                changed_ = true;
                return Result::Closed;
            }
            if (confirmRect(1).contains(p)) {
                if (direct_) return Result::Closed;
                enter(State::Bar, nav);
            }
            return bar_.rect().contains(p) ? Result::Open : Result::Closed;
        case State::Bar:
            break;
    }
    if (!bar_.rect().contains(p)) return Result::Closed;
    if (readOnly()) return Result::Open;  // its line only
    const AnnotationBar::Hit hit = bar_.hit(p);
    if (hit.action == AnnotationBar::Action::Colour) {
        const std::string colour = zoteroColours()[hit.colour].hex;
        // Change only the colour, on the annotation as it is now (a sync may have updated it).
        auto now = nav.library().annotation(annotation_.key);
        if (!now || now->deletedHere) return Result::Closed;
        annotation_ = *now;
        if (colour != annotation_.color) {
            annotation_.color = colour;
            nav.library().editAnnotation(annotation_);
            plugins::annotationEvent(nav.plugins(), nav.library(), "highlightChanged", annotation_.key);
            changed_ = true;
        }
        bar_.setColour(colour);
        nav.damage(bar_.rect(), Waveform::Ui);
    } else if (hit.action == AnnotationBar::Action::Tag) {
        const std::string tag = bar_.tags().chips()[static_cast<size_t>(hit.colour)];
        auto tags = setAnnotationTag(nav, annotation_.key, tag, !bar_.tags().isOn(tag));
        if (!tags) return Result::Closed;
        if (auto now = nav.library().annotation(annotation_.key)) annotation_ = *now;
        changed_ = changed_ || *tags != bar_.tags().on();
        bar_.tags().setOn(*tags);
        nav.damage(bar_.tags().rect(), Waveform::Ui);
    } else if (hit.action == AnnotationBar::Action::AddTag) {
        // The choice lands on the annotation even if this editor has gone meanwhile (a page turn).
        std::weak_ptr<AnnotationEditor*> self = self_;
        Nav* n = &nav;
        const std::string key = annotation_.key;
        pickAnnotationTag(nav, [self, n, key](const std::string& tag) {
            auto tags = setAnnotationTag(*n, key, tag, true);
            auto alive = self.lock();
            AnnotationEditor* e = alive ? *alive : nullptr;
            if (!e) {
                if (tags) n->sync().pushSoon();
                return;
            }
            if (!tags) return;
            if (auto now = n->library().annotation(key)) e->annotation_ = *now;
            e->changed_ = true;
            e->bar_.tags().setChips(recentChips(n->library()));
            e->bar_.tags().setOn(*tags);
        });
    } else if (hit.action == AnnotationBar::Action::Comment) {
        enter(State::Comment, nav);
    } else if (hit.action == AnnotationBar::Action::Delete) {
        enter(State::Confirm, nav);
    }
    return Result::Open;
}

AnnotationEditor::Result AnnotationEditor::onKey(const KeyEvent& k, Nav& nav) {
    if (state_ != State::Comment) return Result::Open;
    if (k.code == KeyEvent::Code::Escape) {
        nav.keyboard().hide();
        return Result::Closed;
    }
    TextField field;
    field.setText(comment_);
    field.apply(k);
    comment_ = field.text();
    if (field.submitted()) {
        auto now = nav.library().annotation(annotation_.key);
        nav.keyboard().hide();
        if (!now || now->deletedHere) return Result::Closed;
        annotation_ = *now;
        annotation_.comment = comment_;
        nav.library().editAnnotation(annotation_);
        plugins::annotationEvent(nav.plugins(), nav.library(), "highlightChanged", annotation_.key);
        changed_ = true;
        return Result::Closed;
    }
    nav.damage(commentBox_, Waveform::Fast);
    return Result::Open;
}

}  // namespace zk
