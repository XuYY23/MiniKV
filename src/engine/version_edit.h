#pragma once

#include "engine/dbformat.h"
#include "engine/table.h"
#include "env.h"
#include "minikv/status.h"

#include <memory>
#include <string>
#include <vector>

namespace minikv {

// 磁盘版本元数据。prev_log_number 在 MemTable 刷盘完成前保留其 WAL。
struct VersionEdit {
  uint64_t next_file_number = 0;
  SequenceNumber last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;
  std::vector<uint64_t> sst_numbers;
};

Status SaveManifest(Env* env, const std::string& dbname, const VersionEdit& edit);
Status LoadManifest(Env* env, const std::string& dbname, VersionEdit* edit,
                    bool* found);

}  // namespace minikv
