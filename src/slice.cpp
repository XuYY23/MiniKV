#include "minikv/slice.h"

#include <algorithm>

namespace minikv {

int Slice::compare(const Slice& b) const {
  const size_t min_len = std::min(size_, b.size_);
  int r = 0;
  if (min_len > 0) {
    r = memcmp(data_, b.data_, min_len);
  }
  if (r == 0) {
    if (size_ < b.size_) {
      r = -1;
    } else if (size_ > b.size_) {
      r = 1;
    }
  }
  return r;
}

}  // namespace minikv
