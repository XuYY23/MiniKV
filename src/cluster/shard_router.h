#pragma once

#include <cstdint>
#include <string>

namespace minikv {
namespace cluster {

using ShardId = uint32_t;

// 静态分片：hash(key) % num_shards（不做再均衡）。
inline uint64_t HashKey(const std::string& key) {
  // FNV-1a 64，稳定、无额外依赖。
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : key) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

inline ShardId ShardOf(const std::string& key, uint32_t num_shards) {
  if (num_shards == 0) {
    return 0;
  }
  return static_cast<ShardId>(HashKey(key) % num_shards);
}

}  // namespace cluster
}  // namespace minikv
