#pragma once
// Page-by-page position in a list (e-ink lists turn pages instead of scrolling, spec §7.6).

#include <cstdint>

namespace zk {

class Pager {
public:
    Pager(int64_t total, int perPage) : total_(total), per_(perPage > 0 ? perPage : 1) {}
    int page() const { return static_cast<int>(offset_ / per_); }
    int pages() const { return total_ <= 0 ? 1 : static_cast<int>((total_ + per_ - 1) / per_); }
    int64_t offset() const { return offset_; }
    int perPage() const { return per_; }
    int64_t total() const { return total_; }
    bool next();
    bool prev();
    void reset() { offset_ = 0; }
    // A new total (the list changed under us): stay on the same page, clamped to the last one.
    void setTotal(int64_t total);

private:
    int64_t total_;
    int per_;
    int64_t offset_ = 0;
};

}  // namespace zk
