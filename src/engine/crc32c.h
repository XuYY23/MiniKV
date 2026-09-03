#pragma once

#include <cstdint>
#include <cstring>

namespace minikv {
namespace crc32c {

// 基于 CRC-32C 多项式的实用实现（查表），用于 WAL 记录完整性校验。
uint32_t Value(const char* data, size_t n);
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Mask(uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
}

inline uint32_t Unmask(uint32_t masked) {
  const uint32_t rot = masked - 0xa282ead8u;
  return (rot >> 17) | (rot << 15);
}

}  // namespace crc32c
}  // namespace minikv
