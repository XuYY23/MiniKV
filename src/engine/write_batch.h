#pragma once

#include "engine/dbformat.h"
#include "minikv/slice.h"
#include "minikv/status.h"

#include <string>

namespace minikv {

class MemTable;

// 一批写操作的编码，作为一条 WAL 记录落盘。
class WriteBatch {
 public:
  // 遍历回调：Put / Delete。
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch();

  void Clear();
  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);

  void SetSequence(SequenceNumber seq);
  SequenceNumber Sequence() const;

  size_t Count() const;
  Slice Contents() const { return Slice(rep_); }
  void SetContents(const Slice& contents);

  // 按编码顺序回调每条操作。
  Status Iterate(Handler* handler) const;

  // 将 batch 应用到 MemTable。
  Status InsertInto(MemTable* mem) const;

  static const size_t kHeader = 12;  // seq(8) + count(4)

 private:
  std::string rep_;
};

}  // namespace minikv
