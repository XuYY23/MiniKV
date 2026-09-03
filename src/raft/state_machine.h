#pragma once

#include "minikv/db.h"
#include "minikv/status.h"
#include "raft/types.h"
#include "raft/node.h"

#include <string>
#include <map>

namespace minikv {
namespace raft {

// 命令编码（写入 Raft log.command）。
// PUT:  "P" + key + '\0' + value
// DEL:  "D" + key
class KvCommand {
 public:
  static std::string EncodePut(const std::string& key, const std::string& value);
  static std::string EncodeDelete(const std::string& key);

  enum class Kind { kPut, kDelete, kUnknown };

  static Kind Parse(const std::string& cmd, std::string* key, std::string* value);
};

// 把已提交命令 Apply 到单机 LSM（非拥有 DB*）。
class KvStateMachine : public StateMachine {
 public:
  explicit KvStateMachine(DB* db);

  Status Apply(LogIndex index, Term term,
               const std::string& command) override;
  Status CreateSnapshot(std::string* data) override;
  Status RestoreSnapshot(const std::string& data) override;

 private:
  struct ValueState {
    bool deleted = false;
    std::string value;
  };
  DB* db_;
  std::map<std::string, ValueState> state_;
};

}  // namespace raft
}  // namespace minikv
