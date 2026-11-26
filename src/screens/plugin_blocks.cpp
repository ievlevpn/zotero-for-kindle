#include "screens/plugin_blocks.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>
#include <utility>

#include <glib.h>

#include "core/log.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;
using K = Block::Kind;

namespace {

constexpr int kButtonH = 110, kInputH = 100, kChipH = 76, kChipGapX = 18, kChipGapY = 18, kChipPad = 34;
constexpr int kCardPad = 32, kDividerH = 30, kLabelH = 48, kEmptyDeckH = 300;
// A deck that fills the screen: at least this tall, else the screen pages as other screens do; a
// long card's "2 / 3" in its bottom corner.
constexpr int kMinDeckH = kEmptyDeckH, kCornerW = 160, kCornerH = 44;
const TextStyle kChipStyle{kSans, 7, 500};
const TextStyle kCardTitle{kSerif, 10.5, 700};
const TextStyle kCardMeta{kSans, 7, 400, false, kSecondary};
const TextStyle kCardBody{kSerif, 8.5, 400};
const TextStyle kBadge{kSans, 6, 700};
const TextStyle kNotice{kSans, 7.5, 400, true, kSecondary};
const TextStyle kInputText{kSans, 9, 400};
const TextStyle kRowTitle{kSans, 9.5, 500};
const TextStyle kRowSub{kSans, 7, 400, false, kSecondary};
const TextStyle kRowMeta{kSans, 7.5, 400, false, kTertiary};
const TextStyle kEmpty{kSerif, 10, 400, false, kSecondary};
// A quote (0.11): a passage set apart by a rule on its left, not by a box (the reader's own
// highlights are boxes enough); at most 5 lines, so a long highlight can't take Home's body.
const TextStyle kQuoteText{kSerif, 9, 400, true};
const TextStyle kQuoteSource{kSans, 7, 400, false, kSecondary};
constexpr int kQuoteRuleW = 6, kQuoteIndent = kQuoteRuleW + 30, kQuoteLines = 5, kQuoteSourceGap = 14, kQuotePadY = 4;

int gapPx(Block::Gap g) {
    switch (g) {
        case Block::Gap::None: return 0;
        case Block::Gap::XSmall: return 6;
        case Block::Gap::Small: return 18;
        case Block::Gap::Medium: return 36;
        case Block::Gap::Large: return 60;
    }
    return 18;
}

int spacePx(Block::Gap g) { return g == Block::Gap::Small ? 18 : g == Block::Gap::Large ? 72 : 36; }

TextStyle textStyle(Block::Style s) {
    TextStyle st;
    switch (s) {
        case Block::Style::Title: st = {kSerif, 11, 700}; break;
        case Block::Style::Body: st = {kSerif, 9, 400}; break;
        case Block::Style::Meta: st = {kSans, 7, 400, false, kSecondary}; break;
        case Block::Style::Label: st = {kSans, 6.5, 700, false, kSecondary}; break;
    }
    st.markup = true;
    return st;
}

// ── 0.12: a box, and text styled by its options ──
//
// A box's border is ink, its fill a light grey or ink; text inside an ink box is white unless its
// tone says otherwise. While a box is laid out or drawn, gOnInk says whether its inside is ink.
constexpr int kSmallRadius = 8;
bool gOnInk = false;
struct OnInk {
    bool was = gOnInk;
    explicit OnInk(const Block& box) {
        if (box.fill != Block::Fill::None) gOnInk = box.fill == Block::Fill::Ink;
    }
    ~OnInk() { gOnInk = was; }
    OnInk(const OnInk&) = delete;
    OnInk& operator=(const OnInk&) = delete;
};
int borderPx(Block::Border b) { return b == Block::Border::Thin ? 2 : b == Block::Border::Thick ? 3 : 0; }
int boxInset(const Block& box) { return borderPx(box.border) + gapPx(box.pad); }
uint8_t fillGray(Block::Fill f) { return f == Block::Fill::Ink ? 28 : 214; }
double boxRadius(const Block& box, Rect r) {
    return box.radius == Block::Radius::Small ? kSmallRadius : box.radius == Block::Radius::Round ? r.h / 2.0 : 0;
}

// A text block's style: its preset (`style`), overridden by the options it gives.
TextStyle textStyleOf(const Block& b) {
    TextStyle st = textStyle(b.style);
    if (b.font) st.family = *b.font == Block::Font::Serif ? kSerif : kSans;
    if (b.size) {
        static constexpr double kPt[] = {6, 7, 9, 11, 13};  // XSmall, Small, Body, Large, Title
        st.size_pt = kPt[static_cast<int>(*b.size)];
    }
    if (b.weight) {
        static constexpr int kWeight[] = {400, 500, 700};  // Regular, Medium, Bold
        st.weight = kWeight[static_cast<int>(*b.weight)];
    }
    if (b.italic) st.italic = true;
    switch (b.tone.value_or(Block::Tone::Auto)) {
        case Block::Tone::Ink: st.gray = kInk; break;
        case Block::Tone::Secondary: st.gray = kSecondary; break;
        case Block::Tone::Inverse: st.gray = 255; break;
        case Block::Tone::Auto:
            if (gOnInk) st.gray = 255;
            break;
    }
    return st;
}

Align alignOf(const Block& b) {
    switch (b.textAlign.value_or(Block::TextAlign::Start)) {
        case Block::TextAlign::Start: return Align::Left;
        case Block::TextAlign::Center: return Align::Center;
        case Block::TextAlign::End: return Align::Right;
    }
    return Align::Left;
}

std::string upper(const std::string& s) {
    gchar* up = g_utf8_strup(s.c_str(), static_cast<gssize>(s.size()));
    std::string out = up;
    g_free(up);
    return out;
}

// The text's spans as markup; caps: uppercase, tracked 0.12 em (a section label).
std::string markupOf(const Block& b) {
    std::string m;
    for (const auto& s : b.spans) {
        std::string t = markupEscape(b.caps ? upper(s.text) : s.text);
        if (s.italic) t = "<i>" + t + "</i>";
        if (s.bold) t = "<b>" + t + "</b>";
        m += t;
    }
    if (b.caps && !m.empty())
        m = "<span letter_spacing=\"" + std::to_string(static_cast<int>(textStyleOf(b).size_pt * 0.12 * 1024)) + "\">" + m + "</span>";
    return m;
}

// ── Text blocks, laid out once ──
//
// Every tree still held keeps its text blocks' layouts (Laid::kept); a layout is found again by its
// text, style, line limit and width. While layoutTree runs, the ones it has used are pinned here.
int64_t gTextLayouts = 0;
std::map<std::string, std::weak_ptr<const KeptText>>& keptTexts() {
    static std::map<std::string, std::weak_ptr<const KeptText>> m;
    return m;
}
std::vector<std::shared_ptr<const KeptText>>* gPins = nullptr;
std::map<std::string, std::weak_ptr<const KeptMathText>>& keptMathTexts() {
    static std::map<std::string, std::weak_ptr<const KeptMathText>> m;
    return m;
}
std::vector<std::shared_ptr<const KeptMathText>>* gMathPins = nullptr;

std::shared_ptr<const KeptMathText> keptMathText(std::string_view utf8, const TextStyle& st, int w, int lines, Align align) {
    std::string key = std::string(st.family) + ' ' + std::to_string(st.size_pt) + ' ' + std::to_string(st.weight) + ' ' +
                      std::to_string(st.italic) + ' ' + std::to_string(st.gray) + ' ' + std::to_string(static_cast<int>(align)) + ' ' +
                      std::to_string(lines) + ' ' + std::to_string(w) + ' ' + std::string(utf8);
    auto& cache = keptMathTexts();
    if (auto it = cache.find(key); it != cache.end())
        if (auto t = it->second.lock()) {
            if (gMathPins) gMathPins->push_back(t);
            return t;
        }
    for (auto it = cache.begin(); it != cache.end();)
        it = it->second.expired() ? cache.erase(it) : std::next(it);
    ++gTextLayouts;
    auto t = std::make_shared<const KeptMathText>(utf8, st, std::max(1, w), std::max(0, lines), align);
    cache[std::move(key)] = t;
    if (gMathPins) gMathPins->push_back(t);
    return t;
}

bool textUsesMath(const Block& b) { return b.math && hasMath(plainText(b)); }

std::shared_ptr<const KeptText> keptText(const Block& b, int w) {
    std::string m = markupOf(b);
    const TextStyle st = textStyleOf(b);
    std::string key = std::string(st.family) + ' ' + std::to_string(st.size_pt) + ' ' + std::to_string(st.weight) + ' ' +
                      std::to_string(st.italic) + ' ' + std::to_string(st.gray) + ' ' + std::to_string(static_cast<int>(alignOf(b))) +
                      ' ' + std::to_string(b.lines) + ' ' + std::to_string(w) + ' ' + m;
    auto& cache = keptTexts();
    if (auto it = cache.find(key); it != cache.end())
        if (auto t = it->second.lock()) {
            if (gPins) gPins->push_back(t);
            return t;
        }
    for (auto it = cache.begin(); it != cache.end();)  // the layouts no tree holds any more
        it = it->second.expired() ? cache.erase(it) : std::next(it);
    ++gTextLayouts;
    auto t = std::make_shared<const KeptText>(m, st, std::max(1, w), std::max(0, b.lines), alignOf(b));
    cache[std::move(key)] = t;
    if (gPins) gPins->push_back(t);
    return t;
}

int textH(const std::string& s, const TextStyle& st, int w, int lines) {
    return measureText(s.empty() ? " " : s, st, std::max(1, w), lines).height;
}

// ── Items, resolved once per layout ──
ItemSource* gSource = nullptr;
struct Resolved {
    std::vector<ItemRow> rows;
    std::vector<std::string> labels, metas;
};
std::map<const Block*, Resolved>* gResolved = nullptr;

const Resolved& resolved(const Block& b) {
    static const Resolved none;
    if (!gSource || !gResolved) return none;
    if (auto it = gResolved->find(&b); it != gResolved->end()) return it->second;
    Resolved r;
    for (const ItemRef& ref : b.items)
        if (auto row = gSource->row(ref)) {
            r.metas.push_back(b.itemStyle == Block::ItemStyle::Card    ? gSource->cardMeta(*row)
                              : b.itemStyle == Block::ItemStyle::Shelf ? shelfMeta(*row)
                                                                       : std::string());
            r.rows.push_back(std::move(*row));
            r.labels.push_back(ref.label);
        }
    return (*gResolved)[&b] = std::move(r);
}

// ── Images (0.12): a PNG from the plugin's package, in greys ──
//
// Kept while any laid tree holds one (Laid::image), found again by its path; while layoutTree runs,
// the ones it has used are held per block (as items are), so a layout reads each file at most once.
}  // namespace

struct PluginImage {
    cairo_surface_t* surface = nullptr;  // ARGB32, premultiplied, every pixel grey
    int w = 0, h = 0;
    PluginImage() = default;
    PluginImage(const PluginImage&) = delete;
    PluginImage& operator=(const PluginImage&) = delete;
    ~PluginImage() {
        if (surface) cairo_surface_destroy(surface);
    }
};

namespace {

constexpr uintmax_t kMaxImageBytes = 2u << 20;
// What a PNG may decode to: a small file can hold a huge picture, so its header is read first.
constexpr uint64_t kMaxImagePixels = 4u << 20;
constexpr uint32_t kMaxImageSide = 4096;
const std::string* gPackage = nullptr;
std::map<const Block*, std::shared_ptr<const PluginImage>>* gImages = nullptr;

// The PNG at `path` in greys (alpha kept), or null and *why.
std::shared_ptr<const PluginImage> loadGreyPng(const std::string& path, std::string* why) {
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::file_size(path, ec);
    if (ec) return *why = "not found", nullptr;
    if (bytes > kMaxImageBytes) return *why = "over 2 MB", nullptr;
    {  // the signature, then IHDR's width and height (big-endian), before any decoding
        unsigned char head[24] = {};
        std::ifstream in(path, std::ios::binary);
        in.read(reinterpret_cast<char*>(head), sizeof head);
        static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (in.gcount() != sizeof head || std::memcmp(head, kSig, 8) != 0 || std::memcmp(head + 12, "IHDR", 4) != 0)
            return *why = "not a PNG", nullptr;
        const auto be = [&](int at) {
            return uint32_t{head[at]} << 24 | uint32_t{head[at + 1]} << 16 | uint32_t{head[at + 2]} << 8 | head[at + 3];
        };
        const uint32_t w = be(16), h = be(20);
        if (w > kMaxImageSide || h > kMaxImageSide || uint64_t{w} * h > kMaxImagePixels)
            return *why = std::to_string(w) + "x" + std::to_string(h) + " px: over 4 megapixels or 4096 px a side", nullptr;
    }
    cairo_surface_t* png = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(png) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(png);
        return *why = "not a PNG that can be read", nullptr;
    }
    auto img = std::make_shared<PluginImage>();
    img->w = cairo_image_surface_get_width(png);
    img->h = cairo_image_surface_get_height(png);
    if (img->w <= 0 || img->h <= 0) {
        cairo_surface_destroy(png);
        return *why = "empty", nullptr;
    }
    img->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img->w, img->h);
    cairo_t* cr = cairo_create(img->surface);
    cairo_set_source_surface(cr, png, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(png);
    cairo_surface_flush(img->surface);
    unsigned char* data = cairo_image_surface_get_data(img->surface);
    const int stride = cairo_image_surface_get_stride(img->surface);
    for (int y = 0; y < img->h; ++y) {
        auto* px = reinterpret_cast<uint32_t*>(data + static_cast<ptrdiff_t>(y) * stride);
        for (int x = 0; x < img->w; ++x) {
            const uint32_t p = px[x];
            const uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            const uint32_t l = (r * 77 + g * 150 + b * 29) >> 8;  // premultiplied: never over alpha
            px[x] = (p & 0xff000000u) | (l << 16) | (l << 8) | l;
        }
    }
    cairo_surface_mark_dirty(img->surface);
    return img;
}

// An image block's picture: null (no room) when there is no package or its PNG can't be had.
std::shared_ptr<const PluginImage> imageOf(const Block& b) {
    if (!gImages) return nullptr;
    if (auto it = gImages->find(&b); it != gImages->end()) return it->second;
    std::shared_ptr<const PluginImage> img;
    if (gPackage && !gPackage->empty()) {
        static std::map<std::string, std::weak_ptr<const PluginImage>> kept;
        static std::set<std::string> logged;
        const std::string path = *gPackage + "/" + b.src;
        if (auto k = kept.find(path); k != kept.end()) img = k->second.lock();
        if (!img) {
            for (auto it = kept.begin(); it != kept.end();)  // the images no tree holds any more
                it = it->second.expired() ? kept.erase(it) : std::next(it);
            std::string why;
            img = loadGreyPng(path, &why);
            if (img) kept[path] = img;
            else if (logged.insert(path).second) log(LogLevel::Warn, "plugin image %s: %s", path.c_str(), why.c_str());
        }
    }
    return (*gImages)[&b] = img;
}

// ── 0.12's pieces: own widths, covers, progress bars, grids ──

// A cover's, a progress bar's or an image's own width in `w` (what its parent gives it, not a row:
// a row sizes its children itself): a percent of it; an image's auto, its pixels' (at most w).
int ownW(const Block& b, int w) {
    if (b.kind != K::Cover && b.kind != K::Progress && b.kind != K::Image) return w;
    if (b.width == Block::Width::Percent) return std::max(1, w * b.widthPct / 100);
    if (b.kind == K::Image && b.width == Block::Width::Auto)
        if (auto img = imageOf(b)) return std::min(w, img->w);
    return w;
}

constexpr int kProgressH = 6, kStatusBarH = 40;
const TextStyle kStatusBar{kSans, 6.5, 500};
constexpr uint8_t kBlankCover = 214;

int gridColumns(const Block& g, int w) {
    if (g.columns > 0) return g.columns;
    return std::max(1, w / std::max(1, w * g.minWidthPct / 100));
}
int gridCellW(const Block& g, int w) {
    const int n = gridColumns(g, w);
    return std::max(1, (w - gapPx(g.gap) * (n - 1)) / n);
}

// A card is as tall as Home's (cover 400), a gap under it; a row is the app's row.
constexpr int kItemCardH = 400, kItemCardGap = 36;
bool itemCards(const Block& b) { return b.itemStyle == Block::ItemStyle::Card; }
bool shelf(const Block& b) { return b.itemStyle == Block::ItemStyle::Shelf; }
int itemUnitH(const Block& b) { return itemCards(b) ? kItemCardH + kItemCardGap : kRowH; }
int itemH(const Block& b) { return itemCards(b) ? kItemCardH : kRowH; }

// A shelf (0.11): tiles of about kTileW (shelfColumns) in rows that span the width, at least kTileGap apart; a
// tile is its 2:3 cover, the bar and one line (widgets.h, drawItemTile). Its page units are rows.
constexpr int kTileW = 240, kTileMinW = 200, kTileGap = 36, kTileMaxW = 320, kTileRowGap = 40, kTileUnder = 18 + 8 + 12;
const TextStyle kTileLine{kSans, 7, 400, false, kSecondary};
int tileW(int w) {
    const int n = shelfColumns(w);
    return n == 1 ? std::min(w, kTileMaxW) : (w - (n - 1) * kTileGap) / n;
}
int tileH(int w) { return tileW(w) * 3 / 2 + kTileUnder + textH("Xg", kTileLine, 1000, 1); }
int shelfRows(size_t items, int w) {
    const size_t n = static_cast<size_t>(shelfColumns(w));
    return static_cast<int>((items + n - 1) / n);
}

int rowH(const Block& list) {
    for (const auto& r : list.rows)
        if (!r.subtitle.empty()) return 150;
    return 112;
}

int inputLineH() { return textH("Xg", kInputText, 1000, 1); }

// The chips of `b` in lines `w` wide: per chip its line and rect relative to the block's top.
struct ChipPlace {
    int line;
    Rect rect;
};
std::vector<ChipPlace> chipPlaces(const Block& b, int w) {
    std::vector<ChipPlace> out;
    int x = 0, line = 0;
    for (const auto& c : b.chips) {
        const int cw = std::min(w, measureText(c.title.empty() ? " " : c.title, kChipStyle, w, 1).width + 2 * kChipPad);
        if (x > 0 && x + cw > w) x = 0, ++line;
        out.push_back({line, {x, line * (kChipH + kChipGapY), cw, kChipH}});
        x += cw + kChipGapX;
    }
    return out;
}

struct CardHead {
    int titleH = 0, metaH = 0, badgeW = 0, head = 0;
};
CardHead cardHead(const Block& card, int w) {
    CardHead h;
    const int inner = w - 2 * kCardPad;
    if (!card.badge.empty()) h.badgeW = std::min(inner / 2, measureText(card.badge, kBadge, inner, 1).width + 36);
    const int titleW = inner - (h.badgeW ? h.badgeW + 20 : 0);
    h.titleH = card.math && hasMath(card.title)
                   ? keptMathText(card.title, kCardTitle, titleW, 3, Align::Left)->height()
                   : textH(card.title, kCardTitle, titleW, 3);
    h.metaH = card.meta.empty() ? 0 : textH(card.meta, kCardMeta, inner, 1) + 6;
    h.head = kCardPad + h.titleH + h.metaH + (card.body.empty() ? 0 : 24);
    return h;
}

// A block's height as page units: a fixed head and tail around units that may be split over pages.
struct Units {
    int head = 0, tail = 0;
    std::vector<int> h;
    bool breakable() const { return !h.empty(); }
    int sum() const { return std::accumulate(h.begin(), h.end(), 0); }
    int natural() const { return head + sum() + tail; }
};

int naturalH(const Block& b, int w);
Units unitsOf(const Block& b, int w);
Laid place(const Block& b, Rect r, int first, int count);

// A child of anything but a row (a column's, a box's, an item piece's, a grid cell): sized by its own width.
Units childUnits(const Block& c, int w) { return unitsOf(c, ownW(c, w)); }
Laid placeChild(const Block& c, Rect r, int first, int count) {
    r.w = ownW(c, r.w);
    return place(c, r, first, count);
}

// The height of each of a grid's rows (all of them, or at most `rows`): its tallest cell's, cells `cellW` wide.
std::vector<int> gridRowHs(const Block& g, int w) {
    const size_t n = static_cast<size_t>(gridColumns(g, w));
    const int cellW = gridCellW(g, w);
    std::vector<int> hs;
    for (size_t i = 0; i < g.children.size(); ++i) {
        if (i % n == 0) {
            if (g.maxRows > 0 && static_cast<int>(hs.size()) == g.maxRows) break;
            hs.push_back(0);
        }
        hs.back() = std::max(hs.back(), childUnits(g.children[i], cellW).natural());
    }
    return hs;
}

std::vector<int> diffs(const std::vector<int>& tops) {
    std::vector<int> h;
    for (size_t i = 0; i + 1 < tops.size(); ++i) h.push_back(tops[i + 1] - tops[i]);
    return h;
}

// A block's natural width (an `auto` child of a row's), at most `w`.
int naturalW(const Block& b, int w) {
    w = std::max(1, w);
    switch (b.kind) {
        case K::Text:
            if (textUsesMath(b)) return std::min(w, keptMathText(plainText(b), textStyleOf(b), w, 0, alignOf(b))->width());
            return std::min(w, measureText(markupOf(b), textStyleOf(b), w, 0).width);
        case K::Button: return std::min(w, std::max(200, measureText(b.title, {kSans, 8.5, 700}, w, 1).width + 100));
        case K::Box: {
            const int in = 2 * boxInset(b);
            return std::min(w, (b.children.empty() ? 0 : naturalW(b.children[0], w - in)) + in);
        }
        case K::Row: {
            int sum = gapPx(b.gap) * std::max(0, static_cast<int>(b.children.size()) - 1);
            for (const auto& c : b.children) sum += naturalW(c, w);
            return std::min(w, sum);
        }
        case K::Column: {
            int most = 0;
            for (const auto& c : b.children) most = std::max(most, naturalW(c, w));
            return most;
        }
        case K::Spacer: return 0;
        case K::Item: return b.children.empty() ? 0 : naturalW(b.children[0], w);
        case K::Image: {
            auto img = imageOf(b);
            return img ? std::min(w, img->w) : 0;
        }
        default: return w;
    }
}

std::vector<int> defaultWidths(const Block& row, int w);

// The widths of a row's children (0.12): a percent of the room the gaps leave, an auto its natural
// width, fill children share the rest. Every fill child keeps at least kMinShare (or its equal share
// of a narrow row), and a percent child as much of that as it asks for, so text beside a long auto
// never wraps a character a line. Autos over what that leaves: the longest are cut to share it.
// Percents over what the autos leave: they shrink, in proportion, to it.
// Unsized, a button is auto and anything else fill; a row with no sized child is laid out as before
// 0.12 (defaultWidths).
std::vector<int> rowWidths(const Block& row, int w) {
    const auto sized = [](const Block& c) { return c.width != Block::Width::Default; };
    if (std::none_of(row.children.begin(), row.children.end(), sized)) return defaultWidths(row, w);
    const int n = static_cast<int>(row.children.size());
    const int room = std::max(0, w - gapPx(row.gap) * (n - 1));
    std::vector<int> ws(n, 0);
    int autos = 0, percents = 0, fills = 0;
    const auto widthOf = [](const Block& c) {
        if (c.width != Block::Width::Default) return c.width;
        return c.kind == K::Button ? Block::Width::Auto : Block::Width::Fill;
    };
    for (int i = 0; i < n; ++i) {
        const Block& c = row.children[i];
        switch (widthOf(c)) {
            case Block::Width::Auto: autos += ws[i] = naturalW(c, room); break;
            case Block::Width::Percent: percents += ws[i] = c.widthPct * room / 100; break;
            default: ++fills; break;
        }
    }
    constexpr int kMinShare = 120;
    const int least = std::min(kMinShare, room / std::max(1, n));
    int fillsKeep = fills * least, percentsKeep = 0;
    for (int i = 0; i < n; ++i)
        if (widthOf(row.children[i]) == Block::Width::Percent) percentsKeep += std::min(ws[i], least);
    const int autoRoom = std::max(0, room - fillsKeep - percentsKeep);
    if (autos > autoRoom) {  // the short autos keep their width, the longest give way
        std::vector<int> order;
        for (int i = 0; i < n; ++i)
            if (widthOf(row.children[i]) == Block::Width::Auto) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return ws[a] < ws[b]; });
        int left = autoRoom, m = static_cast<int>(order.size());
        for (int i : order) left -= ws[i] = std::min(ws[i], left / m--);
        autos = autoRoom - left;
    }
    if (percents > 0 && percents + autos > room - fillsKeep) {
        const int left = std::max(0, room - fillsKeep - autos), wanted = percents;
        percents = 0;
        for (int i = 0; i < n; ++i)
            if (widthOf(row.children[i]) == Block::Width::Percent) percents += ws[i] = ws[i] * left / wanted;
    }
    const int rest = std::max(0, room - autos - percents);
    for (int i = 0, k = 0; i < n; ++i)
        if (widthOf(row.children[i]) == Block::Width::Fill) ws[i] = rest / fills + (k++ < rest % fills ? 1 : 0);
    return ws;
}

// The widths of a row's children before 0.12 (none sized).
std::vector<int> defaultWidths(const Block& row, int w) {
    const int n = static_cast<int>(row.children.size());
    std::vector<int> ws(n, 0);
    if (!n) return ws;
    const int gap = gapPx(row.gap);
    int fixed = 0, flex = 0, buttons = 0;
    for (const auto& c : row.children) (c.kind == K::Button ? buttons : flex)++;
    // Buttons take their width, but leave every other block at least kMinFlex (text never wraps a
    // character a line).
    constexpr int kMinFlex = 240;
    const int minFlex = std::min(kMinFlex, w / n);
    const int buttonCap = buttons ? std::max(1, (w - flex * minFlex - gap * (n - 1)) / buttons) : 0;
    for (int i = 0; i < n; ++i) {
        if (row.children[i].kind != K::Button) continue;
        ws[i] = std::min({w / n, buttonCap, std::max(200, measureText(row.children[i].title, {kSans, 8.5, 700}, w, 1).width + 100)});
        fixed += ws[i];
    }
    const int rest = std::max(0, w - fixed - gap * (n - 1));
    if (flex == 0) {  // only buttons: right-aligned at their widths
        return ws;
    }
    for (int i = 0, k = 0; i < n; ++i)
        if (row.children[i].kind != K::Button) ws[i] = rest / flex + (k++ < rest % flex ? 1 : 0);
    return ws;
}

Units unitsOf(const Block& b, int w) {
    Units u;
    switch (b.kind) {
        case K::Text: {
            const auto& tops = textUsesMath(b)
                                   ? keptMathText(plainText(b), textStyleOf(b), w, std::max(0, b.lines), alignOf(b))->tops()
                                   : keptText(b, w)->tops();
            if (b.lines > 0) u.head = tops[std::min<size_t>(b.lines, tops.size() - 1)];
            else u.h = diffs(tops);
            break;
        }
        case K::Card:
        case K::Deck: {
            if (b.kind == K::Deck && b.children.empty()) {
                u.head = kEmptyDeckH;
                break;
            }
            const Block& card = b.kind == K::Deck ? b.children[0] : b;
            u.head = cardHead(card, w).head;
            u.tail = kCardPad;
            if (!card.body.empty()) {
                u.h = diffs(card.math && hasMath(card.body)
                                ? keptMathText(card.body, kCardBody, w - 2 * kCardPad, 0, Align::Left)->tops()
                                : lineTops(card.body, kCardBody, w - 2 * kCardPad));
            }
            break;
        }
        case K::Items: {
            const size_t n = resolved(b).rows.size();
            if (!gSource) break;  // no source: no room
            if (n == 0) u.head = b.empty.empty() ? 0 : 112;
            else if (shelf(b)) {
                u.h.assign(static_cast<size_t>(shelfRows(n, w)), tileH(w) + kTileRowGap);
                u.h.back() = tileH(w);
            } else {
                u.h.assign(n, itemUnitH(b));
                u.h.back() = itemH(b);
            }
            break;
        }
        case K::List:
            if (b.rows.empty()) u.head = b.empty.empty() ? 0 : 112;
            else u.h.assign(b.rows.size(), rowH(b));
            break;
        case K::Chips: {
            const auto places = chipPlaces(b, w);
            const int lines = places.empty() ? 0 : places.back().line + 1;
            for (int i = 0; i < lines; ++i) u.h.push_back(kChipH + (i + 1 < lines ? kChipGapY : 0));
            break;
        }
        case K::Grid: {  // its rows are its page units; with `rows`, one whole that never pages
            const auto hs = gridRowHs(b, w);
            const int gap = gapPx(b.gap);
            for (size_t i = 0; i < hs.size(); ++i) u.h.push_back(hs[i] + (i + 1 < hs.size() ? gap : 0));
            if (b.maxRows > 0) {
                u.head = u.sum();
                u.h.clear();
            }
            break;
        }
        default: u.head = naturalH(b, w);
    }
    return u;
}

int naturalH(const Block& b, int w) {
    switch (b.kind) {
        case K::Column: {
            int h = 0;
            for (size_t i = 0; i < b.children.size(); ++i) h += (i ? gapPx(b.gap) : 0) + childUnits(b.children[i], w).natural();
            return h;
        }
        case K::Row: {
            const auto ws = rowWidths(b, w);
            int h = 0;
            for (size_t i = 0; i < b.children.size(); ++i) h = std::max(h, unitsOf(b.children[i], ws[i]).natural());
            return h;
        }
        case K::Search: return kInputH;
        case K::Field: {
            const int box = b.lines <= 1 ? kInputH : 48 + b.lines * inputLineH();
            return (b.label.empty() ? 0 : kLabelH) + box;
        }
        case K::Button:
        case K::Buttons: return kButtonH;
        case K::Divider: return kDividerH;
        case K::Space: return spacePx(b.gap);
        case K::Notice: return textH(b.text, kNotice, w, 3);
        case K::Quote: {
            const int inner = w - kQuoteIndent;
            return 2 * kQuotePadY + textH(b.text, kQuoteText, inner, kQuoteLines) +
                   (b.meta.empty() ? 0 : kQuoteSourceGap + textH(b.meta, kQuoteSource, inner, 1));
        }
        case K::Spacer: return 0;  // its room is given in a stretched column (stack)
        case K::Box: {
            OnInk ink(b);
            const int in = boxInset(b);
            return 2 * in + (b.children.empty() ? 0 : childUnits(b.children[0], std::max(1, w - 2 * in)).natural());
        }
        case K::Cover: return w * 3 / 2;
        case K::Progress: return kProgressH;
        case K::Item: return b.children.empty() ? 0 : childUnits(b.children[0], w).natural();
        case K::Image: {
            auto img = imageOf(b);
            return img ? w * img->h / img->w : 0;
        }
        default: return unitsOf(b, w).natural();
    }
}

// Splits units into pages of at most `budget` px (at least one unit a page).
std::vector<std::pair<int, int>> split(const std::vector<int>& h, int budget) {
    std::vector<std::pair<int, int>> pages;
    int first = 0, used = 0;
    for (int i = 0; i < static_cast<int>(h.size()); ++i) {
        if (i > first && used + h[i] > budget) {
            pages.push_back({first, i - first});
            first = i, used = 0;
        }
        used += h[i];
    }
    pages.push_back({first, static_cast<int>(h.size()) - first});
    return pages;
}

// A column's children top to bottom. A column given more than its natural height (one stretched in
// a row) shares what is left between its spacers.
std::vector<Laid> stack(const std::vector<Block>& kids, Rect r, int gap) {
    std::vector<Units> units;
    int total = 0, spacers = 0;
    for (size_t i = 0; i < kids.size(); ++i) {
        units.push_back(childUnits(kids[i], r.w));
        total += (i ? gap : 0) + units.back().natural();
        if (kids[i].kind == K::Spacer) ++spacers;
    }
    const int spare = spacers ? std::max(0, r.h - total) : 0;
    std::vector<Laid> out;
    int y = r.y;
    for (size_t i = 0, k = 0; i < kids.size(); ++i) {
        if (y >= r.y + r.h) break;
        int h = units[i].natural();
        if (kids[i].kind == K::Spacer) h = spare / spacers + (static_cast<int>(k++) < spare % spacers ? 1 : 0);
        h = std::min(h, r.y + r.h - y);
        out.push_back(placeChild(kids[i], {r.x, y, r.w, h}, 0, static_cast<int>(units[i].h.size())));
        y += h + gap;
    }
    return out;
}

Laid place(const Block& b, Rect r, int first, int count) {
    Laid l;
    l.block = &b;
    l.rect = r;
    l.first = first;
    l.count = count;
    switch (b.kind) {
        case K::Column: l.children = stack(b.children, r, gapPx(b.gap)); break;
        case K::Text:
            if (textUsesMath(b)) l.mathText = keptMathText(plainText(b), textStyleOf(b), r.w, std::max(0, b.lines), alignOf(b));
            else l.text = keptText(b, r.w);
            break;
        case K::Card:
        case K::Deck: {
            if (b.kind == K::Deck && b.children.empty()) break;
            const Block& card = b.kind == K::Deck ? b.children[0] : b;
            if (card.math) {
                const CardHead h = cardHead(card, r.w);
                const int inner = r.w - 2 * kCardPad;
                const int titleW = inner - (h.badgeW ? h.badgeW + 20 : 0);
                if (hasMath(card.title)) l.cardTitleMath = keptMathText(card.title, kCardTitle, titleW, 3, Align::Left);
                if (!card.body.empty() && hasMath(card.body))
                    l.cardBodyMath = keptMathText(card.body, kCardBody, inner, 0, Align::Left);
            }
            break;
        }
        case K::Row: {
            const auto ws = rowWidths(b, r.w);
            const int gap = gapPx(b.gap);
            int total = std::accumulate(ws.begin(), ws.end(), 0) + gap * (static_cast<int>(ws.size()) - 1);
            const bool buttons = std::all_of(b.children.begin(), b.children.end(), [](const Block& c) { return c.kind == K::Button; });
            int x = r.x + (buttons ? std::max(0, r.w - total) : 0);  // a row of buttons only sits on the right
            for (size_t i = 0; i < b.children.size(); ++i) {
                const Units u = unitsOf(b.children[i], ws[i]);
                int h = std::min(u.natural(), r.h), y = r.y;
                switch (b.align) {
                    case Block::Align::Top: break;
                    case Block::Align::Center: y += (r.h - h) / 2; break;
                    case Block::Align::Bottom: y += r.h - h; break;
                    case Block::Align::Stretch: h = r.h; break;
                }
                l.children.push_back(place(b.children[i], {x, y, ws[i], h}, 0, static_cast<int>(u.h.size())));
                x += ws[i] + gap;
            }
            break;
        }
        case K::Box: {  // its child fills it inside the border and padding
            if (b.children.empty()) break;
            OnInk ink(b);
            const int in = boxInset(b);
            const Rect inner{r.x + in, r.y + in, std::max(0, r.w - 2 * in), std::max(0, r.h - 2 * in)};
            const Units u = childUnits(b.children[0], inner.w);
            l.children.push_back(placeChild(b.children[0], inner, 0, static_cast<int>(u.h.size())));
            break;
        }
        case K::Cover:  // 2:3 always: in a stretched row, at the top of its slot
            l.rect.h = std::min(r.h, r.w * 3 / 2);
            l.items = resolved(b).rows;
            break;
        case K::Image: l.image = imageOf(b); break;
        case K::Item: {  // its child fills it; the item's row (if its ref resolved) is the part hit and repainted
            l.items = resolved(b).rows;
            if (!l.items.empty()) l.parts.push_back({0, r, l.items[0]});
            if (b.children.empty()) break;
            const Units u = childUnits(b.children[0], r.w);
            l.children.push_back(placeChild(b.children[0], r, 0, static_cast<int>(u.h.size())));
            break;
        }
        case K::Grid: {  // the rows first..first+count (with `rows`: all it shows), cells cut at the bottom
            const auto hs = gridRowHs(b, r.w);
            const int n = gridColumns(b, r.w), cellW = gridCellW(b, r.w), gap = gapPx(b.gap);
            const int last = b.maxRows > 0 ? static_cast<int>(hs.size()) : std::min<int>(first + count, static_cast<int>(hs.size()));
            int y = r.y;
            for (int row = b.maxRows > 0 ? 0 : first; row < last && y < r.y + r.h; ++row) {
                for (int c = 0; c < n; ++c) {
                    const size_t i = static_cast<size_t>(row) * n + c;
                    if (i >= b.children.size()) break;
                    const Units u = childUnits(b.children[i], cellW);
                    const int h = std::min(u.natural(), r.y + r.h - y);
                    l.children.push_back(placeChild(b.children[i], {r.x + c * (cellW + gap), y, cellW, h}, 0, static_cast<int>(u.h.size())));
                }
                y += hs[row] + gap;
            }
            break;
        }
        case K::Items: {
            const Resolved& res = resolved(b);
            l.items = res.rows;
            l.itemLabels = res.labels;
            l.itemMetas = res.metas;
            if (shelf(b)) {  // the rows first..first+count, each item a tile
                const int n = shelfColumns(r.w), tw = tileW(r.w), th = tileH(r.w);
                const int size = static_cast<int>(res.rows.size());
                for (int i = first * n; i < (first + count) * n && i < size; ++i) {
                    const int col = i % n, row = i / n - first;
                    const int x = n == 1 ? r.x : r.x + (r.w - tw) * col / (n - 1);
                    l.parts.push_back({i, Rect{x, r.y + row * (th + kTileRowGap), tw, th}.intersected(r), res.rows[i]});
                }
                break;
            }
            for (int i = first; i < first + count && i < static_cast<int>(res.rows.size()); ++i)
                l.parts.push_back({i, Rect{r.x, r.y + (i - first) * itemUnitH(b), r.w, itemH(b)}.intersected(r), res.rows[i]});
            break;
        }
        case K::List:
            for (int i = first; i < first + count; ++i)
                l.parts.push_back({i, Rect{r.x, r.y + (i - first) * rowH(b), r.w, rowH(b)}.intersected(r)});
            break;
        case K::Chips: {
            const auto places = chipPlaces(b, r.w);
            const int top = first * (kChipH + kChipGapY);
            for (size_t i = 0; i < places.size(); ++i)
                if (places[i].line >= first && places[i].line < first + count) {
                    const Rect c = places[i].rect;
                    l.parts.push_back({static_cast<int>(i), Rect{r.x + c.x, r.y + c.y - top, c.w, c.h}.intersected(r)});
                }
            break;
        }
        case K::Buttons: {
            const int n = static_cast<int>(b.children.size()), gap = 24;
            for (int i = 0; i < n; ++i) {
                const int x0 = r.x + (r.w + gap) * i / n, x1 = r.x + (r.w + gap) * (i + 1) / n - gap;
                l.parts.push_back({i, {x0, r.y, x1 - x0, std::min(kButtonH, r.h)}});
            }
            break;
        }
        default: break;
    }
    return l;
}

// ── Trimming (0.12, a Home body's root with fit "trim") ──

std::vector<Laid> trimStack(const std::vector<Block>& kids, Rect r, int gap);

// `b` cut to what fits in `r` (it doesn't fit whole): a grid to its rows that fit, a column to its
// children that fit (recursively); nullopt (dropped) when nothing of it fits or it can't be cut. A
// column that would keep only its first child of several (a section's label) is dropped too: no
// orphaned heading at the bottom.
std::optional<Laid> trimmed(const Block& b, Rect r) {
    r.w = ownW(b, r.w);
    switch (b.kind) {
        case K::Grid: {
            const auto hs = gridRowHs(b, r.w);
            const int gap = gapPx(b.gap);
            int rows = 0, h = 0;
            while (rows < static_cast<int>(hs.size()) && h + (rows ? gap : 0) + hs[rows] <= r.h) h += (rows ? gap : 0) + hs[rows++];
            if (rows == 0) return std::nullopt;
            return place(b, {r.x, r.y, r.w, h}, 0, rows);  // its rect ends where the rows do: no row after them is placed
        }
        case K::Column: {
            Laid l;
            l.block = &b;
            l.children = trimStack(b.children, r, gapPx(b.gap));
            // Nothing fits, or only its first child of several (a heading never stands alone at the bottom): dropped.
            if (l.children.empty() || (l.children.size() == 1 && b.children.size() > 1)) return std::nullopt;
            const Rect& last = l.children.back().rect;
            l.rect = {r.x, r.y, r.w, last.y + last.h - r.y};
            return l;
        }
        default: return std::nullopt;
    }
}

// `kids` top to bottom at their natural heights while they fit in `r`; the first that doesn't is
// trimmed (or dropped), and everything after it is dropped.
std::vector<Laid> trimStack(const std::vector<Block>& kids, Rect r, int gap) {
    std::vector<Laid> out;
    int y = r.y;
    for (const Block& c : kids) {
        const int top = out.empty() ? y : y + gap;
        const Units u = childUnits(c, r.w);
        if (top + u.natural() <= r.y + r.h) {
            out.push_back(placeChild(c, {r.x, top, r.w, u.natural()}, 0, static_cast<int>(u.h.size())));
            y = top + u.natural();
            continue;
        }
        if (auto t = trimmed(c, {r.x, top, r.w, std::max(0, r.y + r.h - top)})) out.push_back(std::move(*t));
        break;
    }
    return out;
}

// ── Drawing ──

void border(cairo_t* cr, Rect r, uint8_t g, double radius, double width = 3) {
    gray(cr, g);
    cairo_set_line_width(cr, width);
    roundedRect(cr, r.x + width / 2, r.y + width / 2, r.w - width, r.h - width, radius);
    cairo_stroke(cr);
}

void pill(cairo_t* cr, Rect r, const std::string& text, bool filled) {
    gray(cr, filled ? kInk : 170);
    roundedRect(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, r.h / 2.0);
    if (filled) {
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
    }
    TextStyle st = kChipStyle;
    st.gray = filled ? 255 : kInk;
    centredText(cr, text, st, {r.x + 24, r.y, r.w - 48, r.h});
}

void drawCard(cairo_t* cr, const Block& card, const Laid& l, Rect r, int first, int count, bool deck) {
    const Rect box = r;
    fillRect(cr, {box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 255);
    border(cr, box, deck ? kSecondary : kTertiary, 16, deck ? 4 : 3);  // a deck's card: a firmer edge
    const CardHead h = cardHead(card, box.w);
    const int x = box.x + kCardPad, w = box.w - 2 * kCardPad;
    int y = box.y + kCardPad;
    if (h.badgeW) {
        const Rect b{x + w - h.badgeW, y, h.badgeW, 50};
        fillRect(cr, {b.x, b.y, b.w, b.h}, 235);
        centredText(cr, card.badge, kBadge, {b.x + 18, b.y, b.w - 36, b.h});
    }
    const int titleW = w - (h.badgeW ? h.badgeW + 20 : 0);
    if (l.cardTitleMath) {
        const int n = std::max(0, static_cast<int>(l.cardTitleMath->tops().size()) - 1);
        l.cardTitleMath->drawLines(cr, {x, y, titleW, h.titleH + 4}, 0, std::min(3, n));
    } else {
        drawText(cr, card.title, kCardTitle, {x, y, titleW, h.titleH + 4}, 3);
    }
    y += h.titleH;
    if (h.metaH) drawText(cr, card.meta, kCardMeta, {x, y + 6, w, h.metaH}, 1);
    if (!card.body.empty()) {
        if (l.cardBodyMath) l.cardBodyMath->drawLines(cr, {x, box.y + h.head, w, box.h}, first, count);
        else drawTextLines(cr, card.body, kCardBody, {x, box.y + h.head, w, box.h}, first, count);
    }
}

void drawRow(cairo_t* cr, const Block& list, const ListRow& row, Rect r) {
    fillRect(cr, r, 255);
    if (row.selected) fillRect(cr, {r.x, r.y + 18, 8, r.h - 36}, kInk);
    const int x = r.x + 34, metaW = row.meta.empty() ? 0 : std::min(200, measureText(row.meta, kRowMeta, 200, 1).width);
    int badgeW = row.badge.empty() ? 0 : std::min(r.w / 3, measureText(row.badge, kBadge, r.w, 1).width + 36);
    const int right = r.x + r.w - (metaW ? metaW + 20 : 0) - (badgeW ? badgeW + 16 : 0);
    TextStyle title = kRowTitle;
    if (row.selected) title.weight = 700;
    const bool two = rowH(list) > 112;
    if (two) {
        centredText(cr, row.title, title, {x, r.y + 14, right - x, r.h / 2 - 4}, Align::Left);
        centredText(cr, row.subtitle, kRowSub, {x, r.y + r.h / 2 - 2, right - x, r.h / 2 - 20}, Align::Left);
    } else {
        centredText(cr, row.title, title, {x, r.y, right - x, r.h}, Align::Left);
    }
    if (metaW) centredText(cr, row.meta, kRowMeta, {r.x + r.w - metaW, r.y, metaW, r.h}, Align::Right);
    if (badgeW) {
        const Rect b{right + 16, r.y + (r.h - 50) / 2, badgeW, 50};
        fillRect(cr, b, 235);
        centredText(cr, row.badge, kBadge, {b.x + 18, b.y, b.w - 36, b.h});
    }
    hRule(cr, r.y + r.h - 2, r.x, r.x + r.w);
}

std::string shownText(const Block& b, const FocusState& f) {
    auto it = f.texts.find(b.id);
    return it != f.texts.end() ? it->second : b.value.value_or("");
}

void drawInput(cairo_t* cr, const Block& b, Rect r, const FocusState& f) {
    Rect box = r;
    if (b.kind == K::Field && !b.label.empty()) {
        drawText(cr, b.label, textStyle(Block::Style::Label), {r.x, r.y + 4, r.w, kLabelH - 8}, 1);
        box.y += kLabelH, box.h -= kLabelH;
    }
    const bool focused = f.focused == b.id;
    const std::string text = shownText(b, f);
    const bool empty = text.empty();
    std::string shown = empty ? (focused ? "|" : b.placeholder) : text + (focused ? "|" : "");
    const TextStyle st{kSans, 9, 400, false, empty && !focused ? kTertiary : kInk};
    if (b.kind == K::Search) {
        border(cr, box, kInk, box.h / 2.0);
        searchIcon(cr, box.x + 54, box.y + box.h / 2.0 - 4, kTertiary);
        centredText(cr, shown, st, {box.x + 96, box.y, box.w - 120, box.h}, Align::Left);
    } else if (b.lines <= 1) {
        border(cr, box, focused ? kInk : kTertiary, 14, focused ? 4 : 3);
        centredText(cr, shown, st, {box.x + 30, box.y, box.w - 60, box.h}, Align::Left);
    } else {
        border(cr, box, focused ? kInk : kTertiary, 14, focused ? 4 : 3);
        drawText(cr, shown, st, {box.x + 30, box.y + 24, box.w - 60, box.h - 48}, b.lines);
    }
}

ItemSource* gDrawSource = nullptr;

void drawLaid(cairo_t* cr, const Laid& l, const FocusState& f) {
    const Block& b = *l.block;
    const Rect r = l.rect;
    if (r.empty()) return;
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    switch (b.kind) {
        case K::Column:
        case K::Row:
            for (const auto& c : l.children) drawLaid(cr, c, f);
            break;
        case K::Text: {
            if (l.mathText || textUsesMath(b)) {
                const auto mt = l.mathText ? l.mathText : keptMathText(plainText(b), textStyleOf(b), r.w, std::max(0, b.lines), alignOf(b));
                if (b.lines > 0) mt->drawLines(cr, r, 0, b.lines);
                else mt->drawLines(cr, r, l.first, l.count);
                break;
            }
            const auto text = l.text ? l.text : keptText(b, r.w);
            const auto& tops = text->tops();
            if (b.lines > 0 && tops[std::min<size_t>(b.lines, tops.size() - 1)] > r.h)  // cut short: ellipsized to fit
                drawText(cr, markupOf(b), textStyleOf(b), r, b.lines, alignOf(b));
            else if (b.lines > 0) text->drawLines(cr, r, 0, b.lines);
            else text->drawLines(cr, r, l.first, l.count);
            break;
        }
        case K::Card: drawCard(cr, b, l, r, l.first, l.count, false); break;
        case K::Deck:
            if (!b.children.empty()) {
                drawCard(cr, b.children[0], l, r, l.first, l.count, true);
            } else {
                border(cr, r, 200, 16);
                drawText(cr, b.empty, kEmpty, {r.x + 40, r.y + r.h / 2 - 30, r.w - 80, r.h / 2}, 2, Align::Center);
            }
            break;
        case K::Items:
            if (l.items.empty()) centredText(cr, b.empty, kEmpty, r);
            for (const auto& p : l.parts) {
                if (!p.item) continue;
                if (shelf(b)) drawItemTile(cr, p.rect, *p.item, gDrawSource ? gDrawSource->status(p.item->attachmentKey) : "");
                else if (itemCards(b))
                    drawItemCard(cr, p.rect, *p.item, l.itemLabels[p.index], l.itemMetas[p.index],
                                 gDrawSource ? gDrawSource->status(p.item->attachmentKey) : "");
                else drawItemRow(cr, p.rect, *p.item, gDrawSource ? gDrawSource->status(p.item->attachmentKey) : "");
            }
            break;
        case K::List:
            if (b.rows.empty()) centredText(cr, b.empty, kEmpty, r);
            for (const auto& p : l.parts) drawRow(cr, b, b.rows[p.index], p.rect);
            break;
        case K::Search:
        case K::Field: drawInput(cr, b, r, f); break;
        case K::Chips: {
            auto ov = f.chips.find(b.id);
            const auto& sel = ov != f.chips.end() ? ov->second : b.selected;
            for (const auto& p : l.parts) {
                const Chip& c = b.chips[p.index];
                const bool on = b.select != Block::Select::None && std::find(sel.begin(), sel.end(), c.id) != sel.end();
                pill(cr, p.rect, c.title, on);
            }
            break;
        }
        case K::Button: drawButton(cr, {r.x, r.y, r.w, std::min(r.h, kButtonH)}, b.title, b.primary); break;
        case K::Buttons:
            for (const auto& p : l.parts) drawButton(cr, p.rect, b.children[p.index].title, b.children[p.index].primary);
            break;
        case K::Divider: hRule(cr, r.y + r.h / 2 - 1, r.x, r.x + r.w, kRule); break;
        case K::Space: break;
        case K::Notice: drawText(cr, b.text, kNotice, r, 3); break;
        case K::Quote: {
            fillRect(cr, {r.x, r.y, kQuoteRuleW, r.h}, kInk);
            const int x = r.x + kQuoteIndent, w = r.w - kQuoteIndent;
            const int th = textH(b.text, kQuoteText, w, kQuoteLines);
            drawText(cr, b.text, kQuoteText, {x, r.y + kQuotePadY, w, th}, kQuoteLines);
            if (!b.meta.empty())
                drawText(cr, b.meta, kQuoteSource, {x, r.y + kQuotePadY + th + kQuoteSourceGap, w, r.h}, 1);
            break;
        }
        case K::Box: {
            OnInk ink(b);
            const double radius = boxRadius(b, r);
            if (b.fill != Block::Fill::None) {
                gray(cr, fillGray(b.fill));
                roundedRect(cr, r.x, r.y, r.w, r.h, radius);
                cairo_fill(cr);
            }
            if (b.border != Block::Border::None) border(cr, r, kInk, radius, borderPx(b.border));
            for (const auto& c : l.children) drawLaid(cr, c, f);
            break;
        }
        case K::Spacer: break;
        case K::Cover:
            if (l.items.empty()) fillRect(cr, r, kBlankCover);  // its ref didn't resolve: blank
            else drawCover(cr, r, l.items[0]);
            break;
        case K::Progress: progressBar(cr, {r.x, r.y, r.w, std::min(r.h, kProgressH)}, b.fraction); break;
        case K::Grid:
            for (const auto& c : l.children) drawLaid(cr, c, f);
            break;
        case K::Item: {
            for (const auto& c : l.children) drawLaid(cr, c, f);
            const std::string status = !l.items.empty() && gDrawSource ? gDrawSource->status(l.items[0].attachmentKey) : "";
            if (!status.empty()) {  // a download's progress or error, over the piece's bottom
                const Rect bar = Rect{r.x, r.y + r.h - kStatusBarH, r.w, kStatusBarH}.intersected(r);
                fillRect(cr, bar, 255);
                centredText(cr, status, kStatusBar, {bar.x + 10, bar.y, bar.w - 20, bar.h}, Align::Left);
            }
            break;
        }
        case K::Image:
            if (l.image) {
                cairo_translate(cr, r.x, r.y);
                const double scale = static_cast<double>(r.w) / l.image->w;  // aspect kept; a cut one is cropped
                cairo_scale(cr, scale, scale);
                cairo_set_source_surface(cr, l.image->surface, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
                cairo_paint(cr);
            }
            break;
    }
    cairo_restore(cr);
}

void hitIn(const Laid& l, Point p, std::optional<Hit>& hit) {
    if (!l.rect.contains(p) || hit) return;
    const Block& b = *l.block;
    for (const auto& c : l.children) hitIn(c, p, hit);
    if (hit) return;
    switch (b.kind) {
        case K::Button: hit = Hit{b.id, "", b.kind, l.rect}; break;
        case K::Search:
        case K::Field:
        case K::Deck: hit = Hit{b.id, "", b.kind, l.rect}; break;
        case K::Notice:
        case K::Quote:
        case K::Text:   // a link (0.12)
        case K::Box:    // a custom button, a segment of a switch (0.12)
        case K::Image:  // (0.12)
            if (!b.id.empty()) hit = Hit{b.id, "", b.kind, l.rect};
            break;
        case K::Item:  // the item, as an items block's (0.12); inert when its ref didn't resolve
            if (!l.parts.empty()) hit = Hit{b.id, l.parts[0].item->key, b.kind, l.rect, l.parts[0].item};
            break;
        case K::Buttons:
        case K::Items:
        case K::List:
        case K::Chips:
            for (const auto& part : l.parts)
                if (part.rect.contains(p)) {
                    if (b.kind == K::Items) hit = Hit{b.id, part.item->key, b.kind, part.rect, part.item};
                    else if (b.kind == K::Buttons) hit = Hit{b.children[part.index].id, "", K::Button, part.rect};
                    else if (b.kind == K::List) hit = Hit{b.id, b.rows[part.index].id, b.kind, part.rect};
                    else hit = Hit{b.id, b.chips[part.index].id, b.kind, part.rect};
                }
            break;
        default: break;
    }
}

}  // namespace

int shelfColumns(int width) {
    // As many tiles of about kTileW as fit, rounded (a KT4's 958 px takes 4 of 212, not 3 of 298:
    // a row of 3 is too tall to leave room for anything else), never narrower than kTileMinW.
    int n = std::max(1, (width + kTileGap + (kTileW + kTileGap) / 2) / (kTileW + kTileGap));
    while (n > 1 && (width - (n - 1) * kTileGap) / n < kTileMinW) --n;
    return n;
}

Rect blocksArea() {
    const Metrics& m = metrics();
    return {kMargin, kHeaderH + 12, m.width - 2 * kMargin, m.height - kHeaderH - 12 - 24};
}

static Laid layOut(const Block& root, Rect area, int page) {
    if (root.kind == K::Column && root.fit == Block::Fit::Trim) {  // a Home body: what doesn't fit is left out, one page
        Laid out;
        out.block = &root;
        out.rect = area;
        out.children = trimStack(root.children, area, gapPx(root.gap));
        return out;
    }
    std::vector<const Block*> kids;
    int gap = 0;
    if (root.kind == K::Column) {
        for (const auto& c : root.children) kids.push_back(&c);
        gap = gapPx(root.gap);
    } else {
        kids.push_back(&root);
    }
    const int n = static_cast<int>(kids.size());
    std::vector<Units> units;
    int total = 0;
    for (int i = 0; i < n; ++i) {
        units.push_back(childUnits(*kids[i], area.w));
        total += (i ? gap : 0) + units[i].natural();
    }
    Laid out;
    out.block = &root;
    out.rect = area;

    // A deck fills the screen: it takes every pixel the other blocks leave, so every card (and the
    // empty deck) has the same size and nothing around it moves from card to card. A long card
    // pages inside itself, its page indicator in its bottom corner.
    int deck = -1, others = gap * std::max(0, n - 1);
    for (int i = 0; i < n; ++i) {
        if (kids[i]->kind == K::Deck && deck < 0 && root.kind == K::Column) deck = i;
        else others += units[i].natural();
    }
    if (deck >= 0 && area.h - others >= kMinDeckH) {
        const int deckH = area.h - others;
        const Units& u = units[deck];
        int first = 0, count = static_cast<int>(u.h.size());
        if (u.breakable() && u.natural() > deckH) {  // the body's lines, a page at a time, above the corner
            const auto pages = split(u.h, deckH - u.head - u.tail - kCornerH);
            out.pages = static_cast<int>(pages.size());
            out.page = std::clamp(page, 0, out.pages - 1);
            first = pages[out.page].first, count = pages[out.page].second;
        }
        int y = area.y;
        for (int i = 0; i < n; ++i) {
            const int h = i == deck ? deckH : units[i].natural();
            const Rect r{area.x, y, area.w, h};
            out.children.push_back(i == deck ? place(*kids[i], r, first, count) : placeChild(*kids[i], r, 0, static_cast<int>(units[i].h.size())));
            if (i == deck && out.pages > 1) out.corner = {r.x + r.w - kCardPad - kCornerW, r.y + r.h - kCardPad / 2 - kCornerH, kCornerW, kCornerH};
            y += h + gap;
        }
        return out;
    }

    if (total <= area.h) {  // it all fits
        int y = area.y;
        for (int i = 0; i < n; ++i) {
            out.children.push_back(placeChild(*kids[i], {area.x, y, area.w, units[i].natural()}, 0, static_cast<int>(units[i].h.size())));
            y += units[i].natural() + gap;
        }
        return out;
    }

    const int avail = area.h - kPageIndicatorH;
    int fixed = gap * std::max(0, n - 1), minimum = 0;
    for (const auto& u : units) {
        fixed += u.breakable() ? u.head + u.tail : u.natural();
        if (u.breakable()) minimum += u.h[0];
    }

    if (fixed + minimum <= avail) {
        // Elastic: the blocks that page share what the others leave, smallest needs first.
        std::vector<int> budget(n, 0), order;
        for (int i = 0; i < n; ++i)
            if (units[i].breakable()) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return units[a].sum() < units[b].sum(); });
        // Each gets at least its first unit (a card taller than its share would otherwise push the
        // blocks under it out of the area): what the others still need is held back.
        int room = avail - fixed, left = static_cast<int>(order.size()), reserve = minimum;
        for (int i : order) {
            reserve -= units[i].h[0];
            const int share = std::min(room / left--, room - reserve);
            budget[i] = std::min(units[i].sum(), std::max(units[i].h[0], share));
            room -= budget[i];
        }
        std::vector<std::vector<std::pair<int, int>>> split_(n);
        for (int i : order) {
            split_[i] = split(units[i].h, budget[i]);
            out.pages = std::max(out.pages, static_cast<int>(split_[i].size()));
        }
        out.page = std::clamp(page, 0, out.pages - 1);
        int y = area.y;
        for (int i = 0; i < n; ++i) {
            const Units& u = units[i];
            int h = u.natural(), first = 0, count = static_cast<int>(u.h.size());
            if (u.breakable() && split_[i].size() > 1) {
                const auto& pg = split_[i][std::min<size_t>(out.page, split_[i].size() - 1)];
                first = pg.first, count = pg.second;
                h = u.head + budget[i] + u.tail;
            }
            out.children.push_back(placeChild(*kids[i], {area.x, y, area.w, h}, first, count));
            y += h + gap;
        }
        return out;
    }

    // Flow: the screen pages as a whole; a block that pages is split between screen pages (its
    // head and tail on each part), a block taller than a page is cut to it.
    struct Seg {
        int kid, first, count, y, h;
    };
    std::vector<std::vector<Seg>> pages(1);
    int y = 0;
    const auto newPage = [&] {
        pages.emplace_back();
        y = 0;
    };
    for (int i = 0; i < n; ++i) {
        const Units& u = units[i];
        if (!u.breakable()) {
            const int h = std::min(u.natural(), avail);
            if (y > 0 && y + gap + h > avail) newPage();
            const int top = y ? y + gap : 0;
            pages.back().push_back({i, 0, 0, top, h});
            y = top + h;
            continue;
        }
        int at = 0;
        const int count = static_cast<int>(u.h.size());
        while (at < count) {
            const int top = y ? y + gap : 0;
            int room = avail - top - u.head - u.tail, k = 0, used = 0;
            while (at + k < count && used + u.h[at + k] <= room) used += u.h[at + k++];
            if (k == 0) {
                if (y > 0) {
                    newPage();
                    continue;
                }
                k = 1, used = std::min(u.h[at], std::max(0, room));
            }
            pages.back().push_back({i, at, k, top, std::min(avail, u.head + used + u.tail)});
            y = top + pages.back().back().h;
            at += k;
            if (at < count) newPage();
        }
    }
    if (pages.back().empty() && pages.size() > 1) pages.pop_back();
    out.pages = static_cast<int>(pages.size());
    out.page = std::clamp(page, 0, out.pages - 1);
    for (const Seg& s : pages[out.page])
        out.children.push_back(placeChild(*kids[s.kid], {area.x, area.y + s.y, area.w, s.h}, s.first, s.count));
    return out;
}

Laid layoutTree(const Block& root, Rect area, int page, ItemSource* items, const std::string& package) {
    std::vector<std::shared_ptr<const KeptText>> pins;
    std::vector<std::shared_ptr<const KeptMathText>> mathPins;
    std::map<const Block*, Resolved> res;
    std::map<const Block*, std::shared_ptr<const PluginImage>> images;
    auto* outer = std::exchange(gPins, &pins);
    auto* outerMath = std::exchange(gMathPins, &mathPins);
    auto* outerRes = std::exchange(gResolved, &res);
    auto* outerSource = std::exchange(gSource, items);
    auto* outerPackage = std::exchange(gPackage, &package);
    auto* outerImages = std::exchange(gImages, &images);
    Laid out = layOut(root, area, page);
    gPins = outer;
    gMathPins = outerMath;
    gResolved = outerRes;
    gSource = outerSource;
    gPackage = outerPackage;
    gImages = outerImages;
    std::sort(pins.begin(), pins.end());
    pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
    out.kept = std::move(pins);
    std::sort(mathPins.begin(), mathPins.end());
    mathPins.erase(std::unique(mathPins.begin(), mathPins.end()), mathPins.end());
    out.keptMath = std::move(mathPins);
    return out;
}

int64_t textBlockLayouts() { return gTextLayouts; }

Rect pageIndicatorRect(const Laid& root) {
    if (root.pages <= 1) return {};
    if (!root.corner.empty()) return root.corner;
    return {root.rect.x, root.rect.y + root.rect.h - kPageIndicatorH, root.rect.w, kPageIndicatorH};
}

void drawTree(cairo_t* cr, const Laid& root, const FocusState& f, ItemSource* items) {
    fillRect(cr, root.rect, 255);
    auto* outer = std::exchange(gDrawSource, items);
    for (const auto& c : root.children) drawLaid(cr, c, f);
    gDrawSource = outer;
    if (root.pages > 1 && !root.corner.empty()) {  // inside the card: quiet, on the card's white
        centredText(cr, std::to_string(root.page + 1) + " / " + std::to_string(root.pages), {kSans, 6.5, 400, false, kTertiary},
                    root.corner, Align::Right);
    } else if (root.pages > 1) {
        drawPageIndicator(cr, pageIndicatorRect(root), root.page, root.pages);
    }
}

std::optional<Hit> hitTest(const Laid& root, Point p) {
    std::optional<Hit> hit;
    for (const auto& c : root.children) hitIn(c, p, hit);
    return hit;
}

const Laid* findLaid(const Laid& l, const std::string& id) {
    if (l.block && !id.empty() && l.block->id == id && l.block != nullptr) return &l;
    for (const auto& c : l.children)
        if (const Laid* f = findLaid(c, id)) return f;
    return nullptr;
}

}  // namespace zk
