#include "engine/table.h"

#include "engine/coding.h"
#include "engine/crc32c.h"

#include <cassert>
#include <cstring>

namespace minikv {
namespace {

constexpr uint64_t kTableMagic = 0x6d696e696b767374ull;  // "minikvst"
constexpr size_t kFooterSize = 24;

Status AppendWithStatus(WritableFile* file, const Slice& data, uint64_t* offset) {
  Status s = file->Append(data);
  if (s.ok()) {
    *offset += data.size();
  }
  return s;
}

}  // namespace

TableBuilder::TableBuilder(Env* env, const std::string& fname)
    : env_(env), fname_(fname) {
  status_ = env_->NewWritableFile(fname_, &file_);
}

TableBuilder::~TableBuilder() {
  if (!finished_ && file_) {
    Abandon().ok();
  }
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  if (!status_.ok() || file_ == nullptr) {
    return;
  }
  offsets_.push_back(offset_);
  std::string buf;
  PutLengthPrefixedSlice(&buf, key);
  PutLengthPrefixedSlice(&buf, value);
  status_ = AppendWithStatus(file_.get(), buf, &offset_);
}

Status TableBuilder::Finish() {
  if (!status_.ok()) {
    return status_;
  }
  if (finished_) {
    return Status::OK();
  }

  const uint64_t index_offset = offset_;
  for (uint64_t off : offsets_) {
    std::string buf;
    PutFixed64(&buf, off);
    status_ = AppendWithStatus(file_.get(), buf, &offset_);
    if (!status_.ok()) {
      return status_;
    }
  }

  std::string footer;
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, static_cast<uint64_t>(offsets_.size()));
  PutFixed64(&footer, kTableMagic);
  assert(footer.size() == kFooterSize);
  status_ = AppendWithStatus(file_.get(), footer, &offset_);
  if (!status_.ok()) {
    return status_;
  }
  status_ = file_->Sync();
  if (!status_.ok()) {
    return status_;
  }
  status_ = file_->Close();
  finished_ = true;
  file_.reset();
  return status_;
}

Status TableBuilder::Abandon() {
  finished_ = true;
  if (file_) {
    file_->Close().ok();
    file_.reset();
  }
  env_->DeleteFile(fname_).ok();
  return Status::OK();
}

Table::Table(std::unique_ptr<RandomAccessFile> file,
             std::vector<uint64_t> offsets, uint64_t file_size)
    : file_(std::move(file)),
      offsets_(std::move(offsets)),
      file_size_(file_size) {}

Table::~Table() = default;

Status Table::Open(Env* env, const std::string& fname,
                   std::unique_ptr<Table>* table) {
  uint64_t file_size = 0;
  Status s = env->GetFileSize(fname, &file_size);
  if (!s.ok()) {
    return s;
  }
  if (file_size < kFooterSize) {
    return Status::Corruption("sst too small: " + fname);
  }

  std::unique_ptr<RandomAccessFile> file;
  s = env->NewRandomAccessFile(fname, &file);
  if (!s.ok()) {
    return s;
  }

  char footer_buf[kFooterSize];
  Slice footer_slice;
  s = file->Read(file_size - kFooterSize, kFooterSize, &footer_slice,
                 footer_buf);
  if (!s.ok()) {
    return s;
  }
  if (footer_slice.size() != kFooterSize) {
    return Status::Corruption("short footer");
  }

  const uint64_t index_offset = DecodeFixed64(footer_slice.data());
  const uint64_t num_entries = DecodeFixed64(footer_slice.data() + 8);
  const uint64_t magic = DecodeFixed64(footer_slice.data() + 16);
  if (magic != kTableMagic) {
    return Status::Corruption("bad sst magic");
  }
  if (index_offset > file_size - kFooterSize) {
    return Status::Corruption("bad index offset");
  }
  const uint64_t available_index_bytes = file_size - kFooterSize - index_offset;
  if (num_entries > available_index_bytes / 8 ||
      num_entries * 8 != available_index_bytes) {
    return Status::Corruption("bad SST index size");
  }

  std::vector<uint64_t> offsets;
  offsets.reserve(static_cast<size_t>(num_entries));
  const size_t index_bytes = static_cast<size_t>(num_entries) * 8;
  std::string index_buf;
  index_buf.resize(index_bytes);
  if (index_bytes > 0) {
    Slice index_slice;
    s = file->Read(index_offset, index_bytes, &index_slice, &index_buf[0]);
    if (!s.ok()) {
      return s;
    }
    if (index_slice.size() != index_bytes) {
      return Status::Corruption("short index");
    }
    for (uint64_t i = 0; i < num_entries; ++i) {
      const uint64_t entry_offset = DecodeFixed64(index_slice.data() + i * 8);
      if (entry_offset >= index_offset ||
          (!offsets.empty() && entry_offset <= offsets.back())) {
        return Status::Corruption("bad SST entry offset");
      }
      offsets.push_back(entry_offset);
    }
  }

  table->reset(new Table(std::move(file), std::move(offsets), file_size));
  return Status::OK();
}

Status Table::ReadEntry(uint64_t offset, std::string* ikey,
                        std::string* value) const {
  // 先读一段前缀解析长度；internal key + value 通常不大，读 64KB 窗口足够。
  const size_t window = 64 * 1024;
  std::string scratch;
  scratch.resize(window);
  Slice chunk;
  Status s = file_->Read(offset, window, &chunk, &scratch[0]);
  if (!s.ok()) {
    return s;
  }
  Slice input = chunk;
  Slice key_slice;
  Slice val_slice;
  if (!GetLengthPrefixedSlice(&input, &key_slice) ||
      !GetLengthPrefixedSlice(&input, &val_slice)) {
    // 窗口不够则按文件剩余重读（稳健路径）。
    const size_t remain = static_cast<size_t>(file_size_ - offset);
    scratch.resize(remain);
    s = file_->Read(offset, remain, &chunk, &scratch[0]);
    if (!s.ok()) {
      return s;
    }
    input = chunk;
    if (!GetLengthPrefixedSlice(&input, &key_slice) ||
        !GetLengthPrefixedSlice(&input, &val_slice)) {
      return Status::Corruption("bad sst entry");
    }
  }
  ikey->assign(key_slice.data(), key_slice.size());
  value->assign(val_slice.data(), val_slice.size());
  return Status::OK();
}

Status Table::Get(const Slice& user_key, SequenceNumber snapshot,
                  std::string* value, bool* found,
                  SequenceNumber* found_sequence) {
  *found = false;
  if (found_sequence != nullptr) {
    *found_sequence = 0;
  }
  if (offsets_.empty()) {
    return Status::OK();
  }

  // 按 user_key 二分；同 key 多版本中选 sequence<=snapshot 的最新一条。
  int left = 0;
  int right = static_cast<int>(offsets_.size()) - 1;
  int candidate = -1;
  std::string ikey;
  std::string val;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    Status s = ReadEntry(offsets_[static_cast<size_t>(mid)], &ikey, &val);
    if (!s.ok()) {
      return s;
    }
    if (ikey.size() < 8) {
      return Status::Corruption("ikey too short");
    }
    const Slice ukey = ExtractUserKey(ikey);
    const int cmp = ukey.compare(user_key);
    if (cmp < 0) {
      left = mid + 1;
    } else if (cmp > 0) {
      right = mid - 1;
    } else {
      candidate = mid;
      // internal key 按 sequence 降序，更早的下标更新；继续向左找第一条。
      right = mid - 1;
    }
  }

  if (candidate < 0) {
    return Status::OK();
  }

  // 从 candidate 向右扫描同 user_key，找第一个 seq<=snapshot。
  for (int i = candidate; i < static_cast<int>(offsets_.size()); ++i) {
    Status s = ReadEntry(offsets_[static_cast<size_t>(i)], &ikey, &val);
    if (!s.ok()) {
      return s;
    }
    if (ExtractUserKey(ikey).compare(user_key) != 0) {
      break;
    }
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    UnpackSequenceAndType(DecodeFixed64(ikey.data() + ikey.size() - 8), &seq,
                          &type);
    if (seq <= snapshot) {
      *found = true;
      if (found_sequence != nullptr) {
        *found_sequence = seq;
      }
      if (type == kTypeDeletion) {
        return Status::NotFound("deleted");
      }
      value->swap(val);
      return Status::OK();
    }
  }
  return Status::OK();
}

Table::Iterator::Iterator(Table* table) : table_(table) {}

bool Table::Iterator::Valid() const { return valid_; }

void Table::Iterator::SeekToFirst() {
  index_ = 0;
  valid_ = false;
  status_ = Status::OK();
  if (table_->offsets_.empty()) {
    return;
  }
  status_ = table_->ReadEntry(table_->offsets_[0], &key_, &value_);
  valid_ = status_.ok();
}

void Table::Iterator::Next() {
  if (!valid_) {
    return;
  }
  ++index_;
  if (index_ >= table_->offsets_.size()) {
    valid_ = false;
    return;
  }
  status_ = table_->ReadEntry(table_->offsets_[index_], &key_, &value_);
  valid_ = status_.ok();
}

Slice Table::Iterator::key() const { return key_; }

Slice Table::Iterator::value() const { return value_; }

}  // namespace minikv
