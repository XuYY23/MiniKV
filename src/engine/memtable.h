#pragma once

#include "engine/arena.h"
#include "engine/dbformat.h"
#include "engine/skiplist.h"
#include "minikv/status.h"

#include <string>

namespace minikv {

class MemTable {
 public:
  explicit MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Ref() { ++refs_; }
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) {
      delete this;
    }
  }

  size_t ApproximateMemoryUsage();

  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  Status Get(const LookupKey& key, std::string* value);

  struct KeyComparator {
    const InternalKeyComparator comparator{};
    int operator()(const char* a, const char* b) const {
      return comparator(a, b);
    }
  };

  using Table = SkipList<const char*, KeyComparator>;

  class Iterator {
   public:
    explicit Iterator(MemTable* table);
    bool Valid() const;
    void SeekToFirst();
    void Seek(const Slice& k);
    void Next();
    Slice key() const;
    Slice value() const;

   private:
    Table::Iterator iter_;
  };

  Iterator* NewIterator() { return new Iterator(this); }

 private:
  ~MemTable();

  KeyComparator comparator_;
  int refs_;
  Arena arena_;
  Table table_;
};

}  // namespace minikv
