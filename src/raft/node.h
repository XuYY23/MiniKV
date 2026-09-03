#pragma once

#include "minikv/status.h"
#include "raft/storage.h"
#include "raft/transport.h"
#include "raft/types.h"
#include "rpc/message.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace minikv {
namespace raft {

struct RaftConfig {
  NodeId id = 0;
  std::vector<NodeId> peers;
  int election_timeout_min_ms = 150;
  int election_timeout_max_ms = 300;
  int heartbeat_interval_ms = 50;
};

using ApplyCallback =
    std::function<void(LogIndex index, Term term, const std::string& command)>;

class StateMachine {
 public:
  virtual ~StateMachine() = default;
  // Apply must be idempotent: a crash may cause the last committed entry to be
  // delivered again before its applied index is durably recorded.
  virtual Status Apply(LogIndex index, Term term,
                       const std::string& command) = 0;
  virtual Status CreateSnapshot(std::string* data) {
    (void)data;
    return Status::InvalidArgument("state machine does not support snapshots");
  }
  virtual Status RestoreSnapshot(const std::string& data) {
    (void)data;
    return Status::InvalidArgument("state machine does not support snapshots");
  }
};

// 选举 + 复制 / 提交 + 可选持久化。
class RaftNode {
 public:
  // storage 可为 nullptr（纯内存）；非空则构造时需调用 InitStorage() Load。
  RaftNode(RaftConfig config, RaftTransport* transport,
           RaftStorageInterface* storage = nullptr);

  Status InitStorage();  // Load；无 storage 则 OK

  NodeId id() const { return config_.id; }
  Role role() const;
  Term current_term() const;
  NodeId leader_id() const;
  NodeId voted_for() const;
  LogIndex commit_index() const;
  LogIndex last_applied() const;
  LogIndex log_base_index() const;
  LogIndex LastLogIndex() const;
  Term LastLogTerm() const;

  void SetApplyCallback(ApplyCallback cb);
  Status SetStateMachine(StateMachine* state_machine);

  Status Propose(const std::string& command, LogIndex* index);
  Status ConfirmLeadership();
  Status CreateSnapshot();
  Status ChangeMembership(const std::vector<NodeId>& voters);
  std::vector<NodeId> voters() const;

  void Tick(uint64_t now_ms);

  rpc::RaftRequestVoteResp OnRequestVote(const rpc::RaftRequestVoteReq& req);
  rpc::RaftAppendEntriesResp OnAppendEntries(const rpc::RaftAppendEntriesReq& req);
  rpc::RaftInstallSnapshotResp OnInstallSnapshot(
      const rpc::RaftInstallSnapshotReq& req);

  void SetElectionDeadlineForTest(uint64_t deadline_ms);
  uint64_t election_deadline_ms() const;
  void StepDownForTest(Term term);

  bool GetLogEntryForTest(LogIndex index, LogEntry* out) const;
  LogIndex MatchIndexForTest(NodeId peer) const;
  LogIndex NextIndexForTest(NodeId peer) const;
  void AppendLocalForTest(Term term, const std::string& command);

 private:
  void BecomeFollower(Term term, NodeId leader);
  void BecomeCandidate();
  void BecomeLeader();
  void StartElection(uint64_t now_ms);
  void ReplicateToPeers();
  bool ReplicateTo(NodeId peer);
  Status MaybeAdvanceCommit();
  Status MaybeApply();
  void ResetElectionDeadline(uint64_t now_ms);
  bool HasQuorum(const std::set<NodeId>& acknowledgements) const;
  bool IsVoter(NodeId id) const;
  std::vector<NodeId> PeersUnlocked() const;
  Status ApplyConfiguration(const std::string& command);
  Status RebuildConfiguration();
  Term TermAt(LogIndex index) const;
  const LogEntry* EntryAt(LogIndex index) const;
  LogIndex LastLogIndexUnlocked() const;
  bool IsLogUpToDate(LogIndex cand_last_index, Term cand_last_term) const;

  Status PersistHardStateUnlocked();
  Status PersistLogUnlocked();

  RaftConfig config_;
  RaftTransport* transport_;
  RaftStorageInterface* storage_;
  ApplyCallback apply_cb_;
  StateMachine* state_machine_ = nullptr;
  Status storage_error_;

  mutable std::mutex mu_;
  Role role_ = Role::kFollower;
  Term current_term_ = 0;
  NodeId voted_for_ = 0;
  NodeId leader_id_ = 0;
  int votes_granted_ = 0;

  std::vector<LogEntry> entries_;
  LogIndex log_base_index_ = 0;
  Term log_base_term_ = 0;
  Snapshot snapshot_;
  LogIndex commit_index_ = 0;
  LogIndex last_applied_ = 0;
  std::map<NodeId, LogIndex> next_index_;
  std::map<NodeId, LogIndex> match_index_;
  std::set<NodeId> voters_old_;
  std::set<NodeId> voters_new_;
  std::set<NodeId> initial_voters_;

  uint64_t election_deadline_ms_ = 0;
  uint64_t next_heartbeat_ms_ = 0;
  uint64_t last_now_ms_ = 0;
  uint64_t rng_state_ = 0;
};

}  // namespace raft
}  // namespace minikv
