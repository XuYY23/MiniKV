#include "engine/crc32c.h"

namespace minikv {
namespace crc32c {
namespace {

struct Table {
  uint32_t data[256]{};

  Table() {
    // Castagnoli 多项式：0x1EDC6F41（反射形式 0x82F63B78）
    constexpr uint32_t kPoly = 0x82F63B78u;
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) {
        if (c & 1u) {
          c = kPoly ^ (c >> 1);
        } else {
          c >>= 1;
        }
      }
      data[i] = c;
    }
  }
};

const Table& GetTable() {
  static const Table table;
  return table;
}

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  const uint32_t* table = GetTable().data;
  uint32_t c = crc ^ 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    c = table[(c ^ static_cast<uint8_t>(data[i])) & 0xffu] ^ (c >> 8);
  }
  return c ^ 0xffffffffu;
}

uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

}  // namespace crc32c
}  // namespace minikv
