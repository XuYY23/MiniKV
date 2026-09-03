#pragma once

#include "minikv/options.h"
#include "minikv/slice.h"
#include "minikv/status.h"

#include <memory>
#include <string>

namespace minikv {

class DB {
 public:
  DB() = default;
  virtual ~DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // 打开或创建数据库目录。成功时 *dbptr 接管所有权。
  static Status Open(const Options& options, const std::string& name,
                     DB** dbptr);

  virtual Status Put(const WriteOptions& options, const Slice& key,
                     const Slice& value) = 0;
  virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;
  virtual Status Get(const ReadOptions& options, const Slice& key,
                     std::string* value) = 0;

  // 刷盘并关闭相关文件；析构前应调用。
  virtual Status Close() = 0;
};

}  // namespace minikv
