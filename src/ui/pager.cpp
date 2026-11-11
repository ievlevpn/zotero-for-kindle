#include "ui/pager.h"

#include <algorithm>

namespace zk {

int Pager::pageOf(int64_t row) const {
    if (starts_.empty()) return static_cast<int>(std::max<int64_t>(0, row) / per_);
    auto it = std::upper_bound(starts_.begin(), starts_.end(), row);
    return std::max(0, static_cast<int>(it - starts_.begin()) - 1);
}

int Pager::page() const { return pageOf(offset_); }

int Pager::pages() const {
    if (!starts_.empty()) return static_cast<int>(starts_.size());
    return total_ <= 0 ? 1 : static_cast<int>((total_ + per_ - 1) / per_);
}

int Pager::rowsOnPage() const {
    if (starts_.empty()) return per_;
    const size_t p = static_cast<size_t>(page());
    return static_cast<int>((p + 1 < starts_.size() ? starts_[p + 1] : total_) - starts_[p]);
}

bool Pager::goTo(int page) {
    if (page < 0 || page >= pages()) return false;
    offset_ = starts_.empty() ? static_cast<int64_t>(page) * per_ : starts_[static_cast<size_t>(page)];
    return true;
}

bool Pager::next() { return goTo(page() + 1); }

bool Pager::prev() { return page() > 0 && goTo(page() - 1); }

void Pager::setTotal(int64_t total) {
    total_ = total;
    if (!starts_.empty()) return;
    const int64_t last = static_cast<int64_t>(pages() - 1) * per_;
    if (offset_ > last) offset_ = last;
}

void Pager::setStarts(int64_t total, std::vector<int64_t> starts) {
    const int was = page();
    total_ = total;
    starts_ = std::move(starts);
    if (!starts_.empty() && starts_.front() != 0) starts_.insert(starts_.begin(), 0);
    goTo(std::min(was, pages() - 1));
}

}  // namespace zk
