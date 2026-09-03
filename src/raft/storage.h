#pragma once

#include "minikv/status.h"
#include "raft/types.h"

#include <string>
#include <vector>

namespace minikv {
namespace raft {

class RaftStorageInterface {
 public:
  virtual ~RaftStorageInterface() = default;
  virtual Status Load(Term* term, NodeId* voted_for,
                      LogIndex* log_base_index, Term* log_base_term,
                      std::vector<LogEntry>* entries,
                      LogIndex* commit_index, LogIndex* last_applied,
                      Snapshot* snapshot) = 0;
  virtual Status SaveHardState(Term term, NodeId voted_for,
                               LogIndex commit_index,
                               LogIndex last_applied) = 0;
  virtual Status SaveLog(LogIndex log_base_index, Term log_base_term,
                         const std::vector<LogEntry>& entries) = 0;
  virtual Status SaveSnapshot(const Snapshot& snapshot) = 0;
};

// File-backed implementation of the Raft persistence interface.
class RaftStorage : public RaftStorageInterface {
 public:
  explicit RaftStorage(std::string dir);

  const std::string& dir() const { return dir_; }

  Status EnsureDir();

  Status Load(Term* term, NodeId* voted_for, LogIndex* log_base_index,
              Term* log_base_term, std::vector<LogEntry>* entries,
              LogIndex* commit_index, LogIndex* last_applied,
              Snapshot* snapshot) override;

  Status SaveHardState(Term term, NodeId voted_for, LogIndex commit_index,
                       LogIndex last_applied) override;

  Status SaveLog(LogIndex log_base_index, Term log_base_term,
                 const std::vector<LogEntry>& entries) override;
  Status SaveSnapshot(const Snapshot& snapshot) override;

 private:
  std::string HardStatePath() const { return dir_ + "/hard_state"; }
  std::string LogPath() const { return dir_ + "/raft.log"; }
  std::string SnapshotPath() const { return dir_ + "/snapshot"; }
  std::string dir_;
};

}  // namespace raft
}  // namespace minikv
