#include "engine/write_batch.h"

#include "engine/coding.h"
#include "engine/memtable.h"

namespace minikv {
namespace {

void SetCount(std::string* rep, uint32_t n) { EncodeFixed32(&(*rep)[8], n); }

uint32_t GetCount(const std::string& rep) {
  return DecodeFixed32(rep.data() + 8);
}

}  // namespace

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
  EncodeFixed64(&rep_[0], 0);
  EncodeFixed32(&rep_[8], 0);
}

void WriteBatch::SetSequence(SequenceNumber seq) {
  EncodeFixed64(&rep_[0], seq);
}

SequenceNumber WriteBatch::Sequence() const {
  return DecodeFixed64(rep_.data());
}

size_t WriteBatch::Count() const { return GetCount(rep_); }

void WriteBatch::SetContents(const Slice& contents) {
  rep_.assign(contents.data(), contents.size());
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
  SetCount(&rep_, GetCount(rep_) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  SetCount(&rep_, GetCount(rep_) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

Status WriteBatch::Iterate(Handler* handler) const {
  if (handler == nullptr) {
    return Status::InvalidArgument("null handler");
  }
  if (rep_.size() < kHeader) {
    return Status::Corruption("WriteBatch too small");
  }

  SequenceNumber seq = Sequence();
  Slice input(rep_.data() + kHeader, rep_.size() - kHeader);
  uint32_t found = 0;
  const uint32_t count = GetCount(rep_);

  while (!input.empty()) {
    if (input.size() < 1) {
      return Status::Corruption("bad WriteBatch record");
    }
    const ValueType type = static_cast<ValueType>(input[0]);
    input.remove_prefix(1);

    Slice key;
    Slice value;
    if (!GetLengthPrefixedSlice(&input, &key)) {
      return Status::Corruption("bad WriteBatch key");
    }
    if (type == kTypeValue) {
      if (!GetLengthPrefixedSlice(&input, &value)) {
        return Status::Corruption("bad WriteBatch value");
      }
      handler->Put(key, value);
    } else if (type == kTypeDeletion) {
      handler->Delete(key);
    } else {
      return Status::Corruption("unknown WriteBatch tag");
    }
    ++seq;
    ++found;
    (void)seq;
  }

  if (found != count) {
    return Status::Corruption("WriteBatch count mismatch");
  }
  return Status::OK();
}

Status WriteBatch::InsertInto(MemTable* mem) const {
  class MemTableInserter : public Handler {
   public:
    MemTableInserter(MemTable* m, SequenceNumber s) : mem_(m), seq_(s) {}

    void Put(const Slice& key, const Slice& value) override {
      mem_->Add(seq_, kTypeValue, key, value);
      ++seq_;
    }
    void Delete(const Slice& key) override {
      mem_->Add(seq_, kTypeDeletion, key, Slice());
      ++seq_;
    }

   private:
    MemTable* mem_;
    SequenceNumber seq_;
  };

  if (rep_.size() < kHeader) {
    return Status::Corruption("WriteBatch too small");
  }
  MemTableInserter inserter(mem, Sequence());
  return Iterate(&inserter);
}

}  // namespace minikv
