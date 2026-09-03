#include "engine/memtable.h"

#include "engine/coding.h"

#include <cassert>
#include <cstring>

namespace minikv {
namespace {

Slice GetLengthPrefixedSlice(const char* data) {
  uint32_t len = 0;
  const char* p = GetVarint32Ptr(data, data + 5, &len);
  assert(p != nullptr);
  return Slice(p, len);
}

}  // namespace

MemTable::MemTable() : comparator_(), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() { assert(refs_ == 0); }

size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }

void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
  const size_t key_size = key.size();
  const size_t val_size = value.size();
  const size_t internal_key_size = key_size + 8;
  const size_t encoded_len =
      static_cast<size_t>(
          VarintLength(static_cast<uint64_t>(internal_key_size))) +
      internal_key_size +
      static_cast<size_t>(VarintLength(static_cast<uint64_t>(val_size))) +
      val_size;

  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, PackSequenceAndType(s, type));
  p += 8;
  p = EncodeVarint32(p, static_cast<uint32_t>(val_size));
  std::memcpy(p, value.data(), val_size);
  assert(p + val_size == buf + encoded_len);

  table_.Insert(buf);
}

Status MemTable::Get(const LookupKey& lkey, std::string* value) {
  Table::Iterator iter(&table_);
  iter.Seek(lkey.memtable_key());
  if (!iter.Valid()) {
    return Status::NotFound("");
  }

  const char* entry = iter.key();
  uint32_t key_length = 0;
  const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
  assert(key_ptr != nullptr);

  const Slice ikey(key_ptr, key_length);
  if (ExtractUserKey(ikey).compare(lkey.user_key()) != 0) {
    return Status::NotFound("");
  }

  const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
  ValueType type;
  SequenceNumber seq = 0;
  UnpackSequenceAndType(tag, &seq, &type);
  (void)seq;

  if (type == kTypeValue) {
    const Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
    value->assign(v.data(), v.size());
    return Status::OK();
  }
  return Status::NotFound("deleted");
}

MemTable::Iterator::Iterator(MemTable* table) : iter_(&table->table_) {}

bool MemTable::Iterator::Valid() const { return iter_.Valid(); }

void MemTable::Iterator::SeekToFirst() { iter_.SeekToFirst(); }

void MemTable::Iterator::Seek(const Slice& k) {
  // 需要 length-prefixed 形式；调用方应构造完整 memtable key。
  iter_.Seek(k.data());
}

void MemTable::Iterator::Next() { iter_.Next(); }

Slice MemTable::Iterator::key() const {
  return GetLengthPrefixedSlice(iter_.key());
}

Slice MemTable::Iterator::value() const {
  Slice key_slice = GetLengthPrefixedSlice(iter_.key());
  return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
}

}  // namespace minikv
