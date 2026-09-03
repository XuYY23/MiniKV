#include "engine/dbformat.h"

#include <cstring>

namespace minikv {

LookupKey::LookupKey(const Slice& user_key, SequenceNumber sequence) {
  const size_t usize = user_key.size();
  const size_t needed =
      static_cast<size_t>(VarintLength(static_cast<uint64_t>(usize + 8))) +
      usize + 8;
  char* dst = nullptr;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }
  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<uint32_t>(usize + 8));
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(sequence, kTypeValue));
  dst += 8;
  end_ = dst;
}

LookupKey::~LookupKey() {
  if (start_ != space_) {
    delete[] const_cast<char*>(start_);
  }
}

void AppendInternalKey(std::string* result, const Slice& user_key,
                       SequenceNumber seq, ValueType t) {
  result->append(user_key.data(), user_key.size());
  PutFixed64(result, PackSequenceAndType(seq, t));
}

}  // namespace minikv
