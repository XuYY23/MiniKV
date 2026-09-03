#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace minikv {

struct Options {
  // MemTable 近似内存上限（字节），超过后转为 Immutable 并由后台 Flush 成 SSTable。
  size_t write_buffer_size = 4 * 1024 * 1024;

  // Size-Tiered：同一大小分桶内 SST 数量达到该阈值则合并。
  size_t compaction_trigger = 4;

  // 为 true 时不自动 Compaction（测试可用）。
  bool disable_auto_compaction = false;

  // 写 WAL 后是否立刻 fsync。true 更安全，false 吞吐更高。
  bool sync = true;

  // 若目录不存在则创建。
  bool create_if_missing = true;

  // 若目录已存在则报错（用于测试隔离）。
  bool error_if_exists = false;
};

struct ReadOptions {
  // 预留扩展位（如 snapshot）。
};

struct WriteOptions {
  // 未设置时使用 Options::sync；设置后仅覆盖当前写操作。
  std::optional<bool> sync;
};

}  // namespace minikv
