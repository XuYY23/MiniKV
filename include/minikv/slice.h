#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

namespace minikv {

// 非拥有字符串视图，类似 string_view，避免无谓拷贝。
class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
  Slice(const char* s) : data_(s), size_(s == nullptr ? 0 : strlen(s)) {}

  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  char operator[](size_t i) const {
    assert(i < size_);
    return data_[i];
  }

  void clear() {
    data_ = "";
    size_ = 0;
  }

  void remove_prefix(size_t n) {
    assert(n <= size_);
    data_ += n;
    size_ -= n;
  }

  std::string ToString() const { return std::string(data_, size_); }

  int compare(const Slice& b) const;

  bool starts_with(const Slice& x) const {
    return (size_ >= x.size_) && (memcmp(data_, x.data_, x.size_) == 0);
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
  return a.compare(b) == 0;
}

inline bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }

}  // namespace minikv
