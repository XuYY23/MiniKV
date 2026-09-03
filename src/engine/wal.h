#pragma once

#include "env.h"
#include "minikv/slice.h"
#include "minikv/status.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace minikv {

// WAL：追加写日志，崩溃后可回放。
// 物理记录：[checksum:uint32][length:uint32][payload...]
// checksum 覆盖 length+payload（masked crc32c）。
class WalWriter {
 public:
  explicit WalWriter(std::unique_ptr<WritableFile> file);
  Status AddRecord(const Slice& payload);
  Status Sync();
  Status Close();

 private:
  std::unique_ptr<WritableFile> file_;
};

class WalReader {
 public:
  explicit WalReader(std::unique_ptr<SequentialFile> file);

  // 顺序读取下一条完整记录；读到文件末尾返回 false 且 status OK。
  // 截断的尾记录被忽略；中间损坏则返回 Corruption。
  bool ReadRecord(Slice* record, std::string* scratch, Status* status);

 private:
  std::unique_ptr<SequentialFile> file_;
  bool eof_ = false;
};

}  // namespace minikv
