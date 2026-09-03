#include "engine/coding.h"

namespace minikv {

void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  char* ptr = buf;
  while (v >= 0x80) {
    *(ptr++) = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *(ptr++) = static_cast<char>(v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  char* ptr = buf;
  while (v >= 0x80) {
    *(ptr++) = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *(ptr++) = static_cast<char>(v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    const uint32_t byte = static_cast<uint8_t>(*p);
    ++p;
    if (byte & 0x80) {
      result |= ((byte & 0x7f) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    const uint64_t byte = static_cast<uint8_t>(*p);
    ++p;
    if (byte & 0x80) {
      result |= ((byte & 0x7f) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len) || input->size() < len) {
    return false;
  }
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++len;
  }
  return len;
}

char* EncodeVarint32(char* dst, uint32_t v) {
  while (v >= 0x80) {
    *(dst++) = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *(dst++) = static_cast<char>(v);
  return dst;
}

char* EncodeVarint64(char* dst, uint64_t v) {
  while (v >= 0x80) {
    *(dst++) = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *(dst++) = static_cast<char>(v);
  return dst;
}

void EncodeFixed64(char* dst, uint64_t value) {
  std::memcpy(dst, &value, sizeof(value));
}

void EncodeFixed32(char* dst, uint32_t value) {
  std::memcpy(dst, &value, sizeof(value));
}

}  // namespace minikv
