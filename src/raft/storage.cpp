#include "raft/storage.h"

#include "engine/coding.h"
#include "engine/crc32c.h"
#include "env.h"
#include "minikv/slice.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>

namespace minikv {
namespace raft {
namespace {

constexpr uint32_t kHardMagicV1 = 0x48535431;  // HST1
constexpr uint32_t kLogMagicV1 = 0x4c4f4731;   // LOG1
constexpr uint32_t kHardMagic = 0x48535432;    // HST2, CRC protected
constexpr uint32_t kLogMagicV2 = 0x4c4f4732;   // LOG2, CRC protected
constexpr uint32_t kLogMagic = 0x4c4f4733;     // LOG3, base index + CRC
constexpr uint32_t kSnapshotMagic = 0x534e5031;  // SNP1

void AppendChecksum(std::string* data) {
  PutFixed32(data, crc32c::Mask(crc32c::Value(data->data(), data->size())));
}

bool HasValidChecksum(const std::string& data) {
  if (data.size() < 4) return false;
  const uint32_t expected = DecodeFixed32(data.data() + data.size() - 4);
  const uint32_t actual = crc32c::Mask(
      crc32c::Value(data.data(), data.size() - 4));
  return expected == actual;
}

Status ReadAll(Env* env, const std::string& path, std::string* out) {
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(path, &file);
  if (!s.ok()) {
    return s;
  }
  out->clear();
  char buf[4096];
  while (true) {
    Slice frag;
    s = file->Read(sizeof(buf), &frag, buf);
    if (!s.ok()) {
      return s;
    }
    if (frag.empty()) {
      break;
    }
    out->append(frag.data(), frag.size());
  }
  return Status::OK();
}

Status WriteAtomic(Env* env, const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(tmp, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (!s.ok()) {
    return s;
  }
  s = file->Sync();
  if (!s.ok()) {
    return s;
  }
  s = file->Close();
  if (!s.ok()) {
    return s;
  }
  s = env->RenameFile(tmp, path);
  if (!s.ok()) return s;
  const size_t slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  return env->SyncDir(dir);
}

}  // namespace

RaftStorage::RaftStorage(std::string dir) : dir_(std::move(dir)) {}

Status RaftStorage::EnsureDir() {
  Env* env = Env::Default();
  if (env->FileExists(dir_)) {
    return Status::OK();
  }
  return env->CreateDir(dir_);
}

Status RaftStorage::Load(Term* term, NodeId* voted_for,
                         LogIndex* log_base_index, Term* log_base_term,
                         std::vector<LogEntry>* entries, LogIndex* commit_index,
                         LogIndex* last_applied, Snapshot* snapshot) {
  if (term == nullptr || voted_for == nullptr || log_base_index == nullptr ||
      log_base_term == nullptr || entries == nullptr ||
      commit_index == nullptr || last_applied == nullptr || snapshot == nullptr) {
    return Status::InvalidArgument("null out");
  }
  *term = 0;
  *voted_for = 0;
  *commit_index = 0;
  *last_applied = 0;
  *log_base_index = 0;
  *log_base_term = 0;
  entries->clear();
  *snapshot = Snapshot();

  Env* env = Env::Default();
  Status s = EnsureDir();
  if (!s.ok()) {
    return s;
  }

  if (env->FileExists(HardStatePath())) {
    std::string raw;
    s = ReadAll(env, HardStatePath(), &raw);
    if (!s.ok()) {
      return s;
    }
    if (raw.size() < 4 + 8 + 8 + 8 + 8) {
      return Status::Corruption("short hard_state");
    }
    const uint32_t magic = DecodeFixed32(raw.data());
    if (magic == kHardMagic) {
      if (raw.size() != 4 + 8 + 8 + 8 + 8 + 4 || !HasValidChecksum(raw))
        return Status::Corruption("hard_state checksum");
    } else if (magic == kHardMagicV1) {
      if (raw.size() != 4 + 8 + 8 + 8 + 8)
        return Status::Corruption("bad HST1 size");
    } else {
      return Status::Corruption("bad hard_state magic");
    }
    *term = DecodeFixed64(raw.data() + 4);
    *voted_for = DecodeFixed64(raw.data() + 12);
    *commit_index = DecodeFixed64(raw.data() + 20);
    *last_applied = DecodeFixed64(raw.data() + 28);
  }

  if (env->FileExists(SnapshotPath())) {
    std::string raw;
    s = ReadAll(env, SnapshotPath(), &raw);
    if (!s.ok()) return s;
    if (raw.size() < 4 + 8 + 8 + 4 + 4 ||
        DecodeFixed32(raw.data()) != kSnapshotMagic ||
        !HasValidChecksum(raw)) {
      return Status::Corruption("bad Raft snapshot");
    }
    snapshot->last_included_index = DecodeFixed64(raw.data() + 4);
    snapshot->last_included_term = DecodeFixed64(raw.data() + 12);
    const uint32_t len = DecodeFixed32(raw.data() + 20);
    if (raw.size() != 24 + static_cast<size_t>(len) + 4) {
      return Status::Corruption("bad Raft snapshot length");
    }
    snapshot->data.assign(raw.data() + 24, len);
  }

  if (env->FileExists(LogPath())) {
    std::string raw;
    s = ReadAll(env, LogPath(), &raw);
    if (!s.ok()) {
      return s;
    }
    if (raw.size() < 8) {
      return Status::Corruption("short raft.log");
    }
    const uint32_t magic = DecodeFixed32(raw.data());
    size_t checksum_bytes = 0;
    size_t header_bytes = 8;
    if (magic == kLogMagic) {
      checksum_bytes = 4;
      header_bytes = 24;
      if (raw.size() < header_bytes + checksum_bytes || !HasValidChecksum(raw))
        return Status::Corruption("raft.log checksum");
      *log_base_index = DecodeFixed64(raw.data() + 8);
      *log_base_term = DecodeFixed64(raw.data() + 16);
    } else if (magic == kLogMagicV2) {
      checksum_bytes = 4;
      if (raw.size() < 12 || !HasValidChecksum(raw))
        return Status::Corruption("raft.log checksum");
    } else if (magic != kLogMagicV1) {
      return Status::Corruption("bad raft.log magic");
    }
    const uint32_t n = DecodeFixed32(raw.data() + 4);
    Slice in(raw.data() + header_bytes,
             raw.size() - header_bytes - checksum_bytes);
    if (n > in.size() / 9) {
      return Status::Corruption("impossible raft.log entry count");
    }
    entries->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      if (in.size() < 8) {
        return Status::Corruption("truncated log entry header");
      }
      LogEntry e;
      e.term = DecodeFixed64(in.data());
      in.remove_prefix(8);
      Slice cmd;
      if (!GetLengthPrefixedSlice(&in, &cmd)) {
        return Status::Corruption("truncated log command");
      }
      e.command.assign(cmd.data(), cmd.size());
      entries->push_back(std::move(e));
    }
    if (!in.empty()) return Status::Corruption("trailing bytes in raft.log");
  }

  // Snapshot is installed before the compacted log. If a crash happens
  // between the two atomic replacements, trim the still-uncompacted log now.
  if (snapshot->last_included_index > *log_base_index) {
    const LogIndex delta = snapshot->last_included_index - *log_base_index;
    bool boundary_matches = false;
    if (delta <= entries->size()) {
      boundary_matches =
          (delta == 0 ? *log_base_term
                      : (*entries)[static_cast<size_t>(delta - 1)].term) ==
          snapshot->last_included_term;
    }
    if (boundary_matches) {
      entries->erase(entries->begin(),
                     entries->begin() + static_cast<std::ptrdiff_t>(delta));
    } else {
      entries->clear();
    }
    *log_base_index = snapshot->last_included_index;
    *log_base_term = snapshot->last_included_term;
  }
  if (*log_base_index != snapshot->last_included_index ||
      *log_base_term != snapshot->last_included_term) {
    return Status::Corruption("log/snapshot boundary mismatch");
  }
  *commit_index = std::max(*commit_index, snapshot->last_included_index);
  *last_applied = std::max(*last_applied, snapshot->last_included_index);
  const LogIndex last_index =
      *log_base_index + static_cast<LogIndex>(entries->size());
  if (*commit_index > last_index || *last_applied > *commit_index ||
      *commit_index < snapshot->last_included_index ||
      *last_applied < snapshot->last_included_index) {
    return Status::Corruption("invalid Raft commit/applied indexes");
  }
  return Status::OK();
}

Status RaftStorage::SaveHardState(Term term, NodeId voted_for,
                                  LogIndex commit_index,
                                  LogIndex last_applied) {
  Status s = EnsureDir();
  if (!s.ok()) {
    return s;
  }
  std::string raw;
  PutFixed32(&raw, kHardMagic);
  PutFixed64(&raw, term);
  PutFixed64(&raw, voted_for);
  PutFixed64(&raw, commit_index);
  PutFixed64(&raw, last_applied);
  AppendChecksum(&raw);
  return WriteAtomic(Env::Default(), HardStatePath(), raw);
}

Status RaftStorage::SaveLog(LogIndex log_base_index, Term log_base_term,
                            const std::vector<LogEntry>& entries) {
  Status s = EnsureDir();
  if (!s.ok()) {
    return s;
  }
  std::string raw;
  PutFixed32(&raw, kLogMagic);
  PutFixed32(&raw, static_cast<uint32_t>(entries.size()));
  PutFixed64(&raw, log_base_index);
  PutFixed64(&raw, log_base_term);
  for (const auto& e : entries) {
    PutFixed64(&raw, e.term);
    PutLengthPrefixedSlice(&raw, e.command);
  }
  AppendChecksum(&raw);
  return WriteAtomic(Env::Default(), LogPath(), raw);
}

Status RaftStorage::SaveSnapshot(const Snapshot& snapshot) {
  Status s = EnsureDir();
  if (!s.ok()) return s;
  if (snapshot.data.size() > static_cast<size_t>(UINT32_MAX)) {
    return Status::InvalidArgument("snapshot too large");
  }
  std::string raw;
  PutFixed32(&raw, kSnapshotMagic);
  PutFixed64(&raw, snapshot.last_included_index);
  PutFixed64(&raw, snapshot.last_included_term);
  PutFixed32(&raw, static_cast<uint32_t>(snapshot.data.size()));
  raw.append(snapshot.data);
  AppendChecksum(&raw);
  return WriteAtomic(Env::Default(), SnapshotPath(), raw);
}

}  // namespace raft
}  // namespace minikv
