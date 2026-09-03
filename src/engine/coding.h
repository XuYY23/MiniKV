#pragma once

#include "minikv/slice.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace minikv {

// 小端定长整数与长度前缀编码，供 WAL / InternalKey 使用。

inline void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  std::memcpy(buf, &value, sizeof(value));
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  std::memcpy(buf, &value, sizeof(value));
  dst->append(buf, sizeof(buf));
}

inline uint32_t DecodeFixed32(const char* ptr) {
  uint32_t result = 0;
  std::memcpy(&result, ptr, sizeof(result));
  return result;
}

inline uint64_t DecodeFixed64(const char* ptr) {
  uint64_t result = 0;
  std::memcpy(&result, ptr, sizeof(result));
  return result;
}

void PutVarint32(std::string* dst, uint32_t v);
void PutVarint64(std::string* dst, uint64_t v);
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

int VarintLength(uint64_t v);

// 向缓冲区写入，返回写后指针。
char* EncodeVarint32(char* dst, uint32_t v);
char* EncodeVarint64(char* dst, uint64_t v);
void EncodeFixed64(char* dst, uint64_t value);
void EncodeFixed32(char* dst, uint32_t value);

}  // namespace minikv
