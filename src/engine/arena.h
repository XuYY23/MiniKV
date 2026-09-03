#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace minikv {

// 块式分配器：MemTable / SkipList 节点只分配不单独释放，随 Arena 析构回收。
class Arena {
 public:
  Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena();

  char* Allocate(size_t bytes);
  char* AllocateAligned(size_t bytes);

  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<char*> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

}  // namespace minikv
