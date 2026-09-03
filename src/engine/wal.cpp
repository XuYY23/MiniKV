#include "engine/wal.h"

#include "engine/coding.h"
#include "engine/crc32c.h"

#include <cstring>

namespace minikv {
namespace {

constexpr size_t kHeaderSize = 8;  // checksum + length
constexpr uint32_t kMaxRecordSize = 64 * 1024 * 1024;

Status ReadExact(SequentialFile* file, char* dst, size_t requested,
                 size_t* actual) {
  *actual = 0;
  while (*actual < requested) {
    Slice fragment;
    Status s = file->Read(requested - *actual, &fragment, dst + *actual);
    if (!s.ok()) return s;
    if (fragment.empty()) break;
    *actual += fragment.size();
  }
  return Status::OK();
}

}  // namespace

WalWriter::WalWriter(std::unique_ptr<WritableFile> file)
    : file_(std::move(file)) {}

Status WalWriter::AddRecord(const Slice& payload) {
  if (payload.size() > kMaxRecordSize) {
    return Status::InvalidArgument("WAL record exceeds 64 MiB");
  }
  std::string header;
  header.resize(kHeaderSize);
  EncodeFixed32(&header[4], static_cast<uint32_t>(payload.size()));

  uint32_t crc = crc32c::Extend(0, header.data() + 4, 4);
  crc = crc32c::Extend(crc, payload.data(), payload.size());
  EncodeFixed32(&header[0], crc32c::Mask(crc));

  Status s = file_->Append(Slice(header));
  if (!s.ok()) {
    return s;
  }
  if (payload.size() > 0) {
    s = file_->Append(payload);
    if (!s.ok()) {
      return s;
    }
  }
  return file_->Flush();
}

Status WalWriter::Sync() { return file_->Sync(); }

Status WalWriter::Close() { return file_->Close(); }

WalReader::WalReader(std::unique_ptr<SequentialFile> file)
    : file_(std::move(file)) {}

bool WalReader::ReadRecord(Slice* record, std::string* scratch, Status* status) {
  *status = Status::OK();
  if (eof_) {
    return false;
  }

  char header[kHeaderSize];
  size_t header_size = 0;
  Status s = ReadExact(file_.get(), header, kHeaderSize, &header_size);
  if (!s.ok()) {
    *status = s;
    return false;
  }
  if (header_size == 0) {
    eof_ = true;
    return false;
  }
  if (header_size < kHeaderSize) {
    // 尾部截断：视为正常结束。
    eof_ = true;
    return false;
  }

  const uint32_t masked_crc = DecodeFixed32(header);
  const uint32_t length = DecodeFixed32(header + 4);
  if (length > kMaxRecordSize) {
    *status = Status::Corruption("WAL record length exceeds limit");
    return false;
  }

  scratch->resize(length);
  if (length > 0) {
    size_t payload_size = 0;
    s = ReadExact(file_.get(), &(*scratch)[0], length, &payload_size);
    if (!s.ok()) {
      *status = s;
      return false;
    }
    if (payload_size < length) {
      // 截断尾记录。
      eof_ = true;
      return false;
    }
  }

  uint32_t actual = crc32c::Extend(0, header + 4, 4);
  if (length > 0) {
    actual = crc32c::Extend(actual, scratch->data(), length);
  }
  if (crc32c::Mask(actual) != masked_crc) {
    *status = Status::Corruption("WAL checksum mismatch");
    return false;
  }

  *record = Slice(scratch->data(), length);
  return true;
}

}  // namespace minikv
