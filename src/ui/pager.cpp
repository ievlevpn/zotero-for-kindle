#include "ui/pager.h"

namespace zk {

bool Pager::next() {
    if (page() + 1 >= pages()) return false;
    offset_ += per_;
    return true;
}

bool Pager::prev() {
    if (offset_ == 0) return false;
    offset_ -= per_;
    return true;
}

void Pager::setTotal(int64_t total) {
    total_ = total;
    const int64_t last = static_cast<int64_t>(pages() - 1) * per_;
    if (offset_ > last) offset_ = last;
}

}  // namespace zk
