#pragma once
// Page-by-page position in a list (e-ink lists turn pages instead of scrolling, spec §7.6).

#include <cstdint>
#include <vector>

namespace zk {

class Pager {
public:
    Pager(int64_t total, int perPage) : total_(total), per_(perPage > 0 ? perPage : 1) {}
    int page() const;
    int pages() const;
    int64_t offset() const { return offset_; }
    int perPage() const { return per_; }
    int64_t total() const { return total_; }
    // The rows on this page: perPage, fewer on the last, or as the page starts say.
    int rowsOnPage() const;
    bool next();
    bool prev();
    bool goTo(int page);  // false if there is no such page
    void reset() { offset_ = 0; }
    // A new total (the list changed under us): stay on the same page, clamped to the last one.
    void setTotal(int64_t total);
    // Pages of their own sizes (a list with headings): starts[i] is page i's first row, from 0.
    // Stays on the same page number, clamped to the last one. Empty: perPage rows a page again.
    void setStarts(int64_t total, std::vector<int64_t> starts);
    int pageOf(int64_t row) const;  // the page holding that row

private:
    int64_t total_;
    int per_;
    int64_t offset_ = 0;
    std::vector<int64_t> starts_;
};

}  // namespace zk
