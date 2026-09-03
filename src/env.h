#pragma once

#include "minikv/slice.h"
#include "minikv/status.h"

#include <cstdint>
#include <memory>
#include <string>

namespace minikv {

// 顺序可读文件。
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

// 随机读文件（SSTable）。
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const = 0;
};

// 追加写文件。
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  virtual Status Append(const Slice& data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

// POSIX 环境下的最小文件操作集合。
class Env {
 public:
  virtual ~Env() = default;

  static Env* Default();

  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) = 0;
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;
  virtual Status NewAppendableFile(const std::string& fname,
                                   std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& fname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status RenameFile(const std::string& src,
                            const std::string& target) = 0;
  // Sync directory metadata after creating, renaming, or deleting durable files.
  virtual Status SyncDir(const std::string& dirname) = 0;
  virtual Status DeleteFile(const std::string& fname) = 0;
  virtual Status CreateDir(const std::string& name) = 0;
  virtual bool FileIsDirectory(const std::string& name) = 0;
};

}  // namespace minikv
