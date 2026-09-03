#pragma once

#include "engine/table.h"

#include <cstddef>
#include <vector>

namespace minikv {

// Size-Tiered：按文件大小分桶，同一桶内文件数达到阈值则合并。
// inputs 输出 tables 中的下标（按文件号升序，偏旧）。
bool PickSizeTieredCompaction(const std::vector<std::unique_ptr<Table>>& tables,
                              size_t trigger,
                              std::vector<size_t>* inputs);

// 将若干 SSTable 归并为一张新表：同 user_key 只保留最新版本。
Status CompactTables(Env* env, const std::string& output_fname,
                     const std::vector<Table*>& inputs);

// 大小分桶：对 file_size 做 log2 分档。
int SizeTierBucket(uint64_t file_size);

}  // namespace minikv
