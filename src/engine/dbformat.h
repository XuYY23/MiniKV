#pragma once

#include "engine/coding.h"
#include "minikv/slice.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace minikv {

// 值类型：普通值或删除标记。
enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
};

using SequenceNumber = uint64_t;

// 打包 sequence 与 type，占 8 字节。
inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  return (seq << 8) | static_cast<uint8_t>(t);
}

inline void UnpackSequenceAndType(uint64_t packed, SequenceNumber* seq,
                                  ValueType* t) {
  *seq = packed >> 8;
  *t = static_cast<ValueType>(packed & 0xff);
}

// 解析 MemTable 中存储的条目：internal_key | value
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

// 比较两条 internal key（不含 length 前缀）：user_key 升序，sequence 降序。
inline int CompareInternalKey(const Slice& a, const Slice& b) {
  const Slice ua = ExtractUserKey(a);
  const Slice ub = ExtractUserKey(b);
  const int r = ua.compare(ub);
  if (r != 0) {
    return r;
  }
  const uint64_t an = DecodeFixed64(a.data() + a.size() - 8);
  const uint64_t bn = DecodeFixed64(b.data() + b.size() - 8);
  if (an > bn) {
    return -1;
  }
  if (an < bn) {
    return 1;
  }
  return 0;
}

class InternalKeyComparator {
 public:
  int operator()(const char* a, const char* b) const {
    // a/b 指向 length-prefixed internal key 开始处。
    uint32_t a_len = 0;
    uint32_t b_len = 0;
    const char* a_key = GetVarint32Ptr(a, a + 5, &a_len);
    const char* b_key = GetVarint32Ptr(b, b + 5, &b_len);
    assert(a_key != nullptr && b_key != nullptr);

    Slice a_ik(a_key, a_len);
    Slice b_ik(b_key, b_len);
    Slice a_user = ExtractUserKey(a_ik);
    Slice b_user = ExtractUserKey(b_ik);
    int r = a_user.compare(b_user);
    if (r != 0) {
      return r;
    }
    const uint64_t a_num = DecodeFixed64(a_ik.data() + a_ik.size() - 8);
    const uint64_t b_num = DecodeFixed64(b_ik.data() + b_ik.size() - 8);
    if (a_num > b_num) {
      return -1;  // 更大 sequence 更“新”，排序更靠前
    }
    if (a_num < b_num) {
      return 1;
    }
    return 0;
  }
};

// 构造查找用的 memtable key：user_key + 最大 sequence 的 tag。
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber sequence);
  ~LookupKey();

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  // 用于 SkipList 查找的完整 memtable key。
  const char* memtable_key() const { return start_; }

  Slice user_key() const {
    return Slice(kstart_, static_cast<size_t>((end_ - kstart_) - 8));
  }

 private:
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];
};

void AppendInternalKey(std::string* result, const Slice& user_key,
                       SequenceNumber seq, ValueType t);

}  // namespace minikv
