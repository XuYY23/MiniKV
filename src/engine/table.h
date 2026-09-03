#pragma once

#include "engine/dbformat.h"
#include "env.h"
#include "minikv/slice.h"
#include "minikv/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace minikv {

// SSTable 写入器。
// 文件布局：
//   [entries...][index offsets...][footer]
// entry = key_len|internal_key|value_len|value
// footer(24B) = index_offset|num_entries|magic
class TableBuilder {
 public:
  TableBuilder(Env* env, const std::string& fname);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;
  ~TableBuilder();

  // key 为 internal key（user_key + tag）。
  void Add(const Slice& key, const Slice& value);
  Status Finish();
  Status Abandon();
  uint64_t FileSize() const { return offset_; }
  uint64_t NumEntries() const { return offsets_.size(); }

 private:
  Env* env_;
  std::string fname_;
  std::unique_ptr<WritableFile> file_;
  Status status_;
  uint64_t offset_ = 0;
  std::vector<uint64_t> offsets_;
  bool finished_ = false;
};

class Table {
 public:
  ~Table();

  static Status Open(Env* env, const std::string& fname,
                     std::unique_ptr<Table>* table);

  // 按用户 key 查找最新记录。
  // found=true 且 OK：拿到 value；found=true 且 NotFound：删除标记；
  // found=false：本表无该用户 key。
  Status Get(const Slice& user_key, SequenceNumber snapshot, std::string* value,
             bool* found, SequenceNumber* found_sequence = nullptr);

  uint64_t number() const { return number_; }
  void set_number(uint64_t n) { number_ = n; }
  uint64_t file_size() const { return file_size_; }

  class Iterator {
   public:
    explicit Iterator(Table* table);
    bool Valid() const;
    void SeekToFirst();
    void Next();
    Slice key() const;    // internal key
    Slice value() const;  // user value
    Status status() const { return status_; }

   private:
    Table* table_;
    size_t index_ = 0;
    std::string key_;
    std::string value_;
    Status status_;
    bool valid_ = false;
  };

  Iterator* NewIterator() { return new Iterator(this); }

 private:
  Table(std::unique_ptr<RandomAccessFile> file, std::vector<uint64_t> offsets,
        uint64_t file_size);

  Status ReadEntry(uint64_t offset, std::string* ikey, std::string* value) const;

  std::unique_ptr<RandomAccessFile> file_;
  std::vector<uint64_t> offsets_;
  uint64_t file_size_ = 0;
  uint64_t number_ = 0;
};

}  // namespace minikv
