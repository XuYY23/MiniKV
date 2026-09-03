#include "raft/state_machine.h"

#include "minikv/options.h"
#include "engine/coding.h"
#include "minikv/slice.h"

namespace minikv {
namespace raft {

std::string KvCommand::EncodePut(const std::string& key,
                                 const std::string& value) {
  std::string out;
  out.push_back('P');
  out.append(key);
  out.push_back('\0');
  out.append(value);
  return out;
}

std::string KvCommand::EncodeDelete(const std::string& key) {
  std::string out;
  out.push_back('D');
  out.append(key);
  return out;
}

KvCommand::Kind KvCommand::Parse(const std::string& cmd, std::string* key,
                                 std::string* value) {
  if (cmd.empty() || key == nullptr) {
    return Kind::kUnknown;
  }
  if (cmd[0] == 'P') {
    const size_t nul = cmd.find('\0', 1);
    if (nul == std::string::npos) {
      return Kind::kUnknown;
    }
    *key = cmd.substr(1, nul - 1);
    if (value != nullptr) {
      *value = cmd.substr(nul + 1);
    }
    return Kind::kPut;
  }
  if (cmd[0] == 'D') {
    *key = cmd.substr(1);
    if (value != nullptr) {
      value->clear();
    }
    return Kind::kDelete;
  }
  return Kind::kUnknown;
}

KvStateMachine::KvStateMachine(DB* db) : db_(db) {}

Status KvStateMachine::Apply(LogIndex /*index*/, Term /*term*/,
                             const std::string& command) {
  if (db_ == nullptr) {
    return Status::InvalidArgument("null db");
  }
  std::string key;
  std::string value;
  const KvCommand::Kind k = KvCommand::Parse(command, &key, &value);
  WriteOptions w;
  // Raft records last_applied only after this durable write succeeds.
  w.sync = true;
  if (command.empty()) {
    return Status::OK();  // Raft leadership/read barrier.
  }
  if (k == KvCommand::Kind::kPut) {
    Status s = db_->Put(w, key, value);
    if (s.ok()) state_[key] = ValueState{false, value};
    return s;
  }
  if (k == KvCommand::Kind::kDelete) {
    Status s = db_->Delete(w, key);
    if (s.ok()) state_[key] = ValueState{true, std::string()};
    return s;
  }
  return Status::Corruption("unknown kv command");
}

Status KvStateMachine::CreateSnapshot(std::string* data) {
  if (data == nullptr) return Status::InvalidArgument("null snapshot output");
  data->clear();
  PutFixed32(data, 0x4b565331);  // KVS1
  PutFixed32(data, static_cast<uint32_t>(state_.size()));
  for (const auto& item : state_) {
    data->push_back(item.second.deleted ? 1 : 0);
    PutLengthPrefixedSlice(data, item.first);
    PutLengthPrefixedSlice(data, item.second.value);
  }
  return Status::OK();
}

Status KvStateMachine::RestoreSnapshot(const std::string& data) {
  if (db_ == nullptr) return Status::InvalidArgument("null db");
  if (data.size() < 8 || DecodeFixed32(data.data()) != 0x4b565331) {
    return Status::Corruption("bad KV snapshot");
  }
  const uint32_t count = DecodeFixed32(data.data() + 4);
  Slice input(data.data() + 8, data.size() - 8);
  if (count > input.size() / 3) {
    return Status::Corruption("impossible KV snapshot entry count");
  }
  std::map<std::string, ValueState> restored;
  WriteOptions w;
  w.sync = true;
  for (uint32_t i = 0; i < count; ++i) {
    if (input.empty()) return Status::Corruption("truncated KV snapshot");
    if (input[0] != 0 && input[0] != 1) {
      return Status::Corruption("bad KV snapshot value kind");
    }
    const bool deleted = input[0] == 1;
    input.remove_prefix(1);
    Slice key;
    Slice value;
    if (!GetLengthPrefixedSlice(&input, &key) ||
        !GetLengthPrefixedSlice(&input, &value)) {
      return Status::Corruption("truncated KV snapshot entry");
    }
    std::string key_string(key.data(), key.size());
    std::string value_string(value.data(), value.size());
    Status s = deleted ? db_->Delete(w, key_string)
                       : db_->Put(w, key_string, value_string);
    if (!s.ok()) return s;
    restored[key_string] = ValueState{deleted, std::move(value_string)};
  }
  if (!input.empty()) return Status::Corruption("trailing KV snapshot bytes");
  state_ = std::move(restored);
  return Status::OK();
}

}  // namespace raft
}  // namespace minikv
