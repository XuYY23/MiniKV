#include "raft/node.h"

#include "engine/coding.h"
#include "minikv/slice.h"

#include <algorithm>
#include <cstring>

namespace minikv {
namespace raft {
namespace {

uint64_t NextRand(uint64_t* state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x == 0 ? 0x9e3779b97f4a7c15ULL : x;
  return *state;
}

enum class ConfigKind { kNone, kJoint, kFinal };

std::string EncodeConfiguration(ConfigKind kind,
                                const std::set<NodeId>& old_voters,
                                const std::set<NodeId>& new_voters) {
  std::string out("\xffRC", 3);
  out.push_back(kind == ConfigKind::kJoint ? 'J' : 'F');
  PutFixed32(&out, static_cast<uint32_t>(old_voters.size()));
  for (NodeId id : old_voters) PutFixed64(&out, id);
  PutFixed32(&out, static_cast<uint32_t>(new_voters.size()));
  for (NodeId id : new_voters) PutFixed64(&out, id);
  return out;
}

ConfigKind DecodeConfiguration(const std::string& command,
                               std::set<NodeId>* old_voters,
                               std::set<NodeId>* new_voters) {
  if (command.size() < 12 ||
      std::memcmp(command.data(), "\xffRC", 3) != 0 ||
      (command[3] != 'J' && command[3] != 'F')) {
    return ConfigKind::kNone;
  }
  Slice input(command.data() + 4, command.size() - 4);
  auto decode_set = [](Slice* in, std::set<NodeId>* result) {
    if (in->size() < 4) return false;
    const uint32_t count = DecodeFixed32(in->data());
    in->remove_prefix(4);
    if (count > in->size() / 8) return false;
    result->clear();
    for (uint32_t i = 0; i < count; ++i) {
      result->insert(DecodeFixed64(in->data()));
      in->remove_prefix(8);
    }
    return true;
  };
  if (!decode_set(&input, old_voters) ||
      !decode_set(&input, new_voters) || !input.empty() ||
      old_voters->empty()) {
    return ConfigKind::kNone;
  }
  return command[3] == 'J' ? ConfigKind::kJoint : ConfigKind::kFinal;
}

Status EncodeSnapshotEnvelope(const std::set<NodeId>& old_voters,
                              const std::set<NodeId>& new_voters,
                              const std::string& state_data,
                              std::string* output) {
  output->clear();
  PutFixed32(output, 0x52535031);  // RSP1
  PutFixed32(output, static_cast<uint32_t>(old_voters.size()));
  for (NodeId id : old_voters) PutFixed64(output, id);
  PutFixed32(output, static_cast<uint32_t>(new_voters.size()));
  for (NodeId id : new_voters) PutFixed64(output, id);
  PutLengthPrefixedSlice(output, state_data);
  return Status::OK();
}

Status DecodeSnapshotEnvelope(const std::string& input_data,
                              std::set<NodeId>* old_voters,
                              std::set<NodeId>* new_voters,
                              std::string* state_data) {
  if (input_data.size() < 12 ||
      DecodeFixed32(input_data.data()) != 0x52535031) {
    return Status::Corruption("bad Raft snapshot envelope");
  }
  Slice input(input_data.data() + 4, input_data.size() - 4);
  auto decode_set = [](Slice* in, std::set<NodeId>* result) {
    if (in->size() < 4) return false;
    const uint32_t count = DecodeFixed32(in->data());
    in->remove_prefix(4);
    if (count > in->size() / 8) return false;
    result->clear();
    for (uint32_t i = 0; i < count; ++i) {
      result->insert(DecodeFixed64(in->data()));
      in->remove_prefix(8);
    }
    return true;
  };
  Slice state;
  if (!decode_set(&input, old_voters) ||
      !decode_set(&input, new_voters) || old_voters->empty() ||
      !GetLengthPrefixedSlice(&input, &state) || !input.empty()) {
    return Status::Corruption("bad Raft snapshot envelope fields");
  }
  state_data->assign(state.data(), state.size());
  return Status::OK();
}

}  // namespace

RaftNode::RaftNode(RaftConfig config, RaftTransport* transport,
                   RaftStorageInterface* storage)
    : config_(std::move(config)), transport_(transport), storage_(storage) {
  rng_state_ = config_.id * 0x9e3779b97f4a7c15ULL + 1;
  voters_old_.insert(config_.id);
  voters_old_.insert(config_.peers.begin(), config_.peers.end());
  initial_voters_ = voters_old_;
  last_now_ms_ = 0;
  ResetElectionDeadline(/*now_ms=*/0);
}

Status RaftNode::InitStorage() {
  if (storage_ == nullptr) {
    return Status::OK();
  }
  Term term = 0;
  NodeId voted = 0;
  std::vector<LogEntry> entries;
  LogIndex log_base_index = 0;
  Term log_base_term = 0;
  LogIndex commit = 0;
  LogIndex applied = 0;
  Snapshot snapshot;
  Status s = storage_->Load(&term, &voted, &log_base_index, &log_base_term,
                            &entries, &commit, &applied, &snapshot);
  if (!s.ok()) {
    return s;
  }
  std::set<NodeId> snapshot_old;
  std::set<NodeId> snapshot_new;
  std::string snapshot_state;
  if (snapshot.last_included_index != 0) {
    s = DecodeSnapshotEnvelope(snapshot.data, &snapshot_old, &snapshot_new,
                               &snapshot_state);
    if (!s.ok()) return s;
  }
  std::lock_guard<std::mutex> lock(mu_);
  current_term_ = term;
  voted_for_ = voted;
  log_base_index_ = log_base_index;
  log_base_term_ = log_base_term;
  entries_ = std::move(entries);
  snapshot_ = std::move(snapshot);
  if (!snapshot_old.empty()) {
    voters_old_ = std::move(snapshot_old);
    voters_new_ = std::move(snapshot_new);
  }
  commit_index_ = commit;
  last_applied_ = applied;
  role_ = Role::kFollower;
  leader_id_ = 0;
  return RebuildConfiguration();
}

Status RaftNode::PersistHardStateUnlocked() {
  if (storage_ == nullptr) {
    return Status::OK();
  }
  Status s = storage_->SaveHardState(current_term_, voted_for_, commit_index_,
                                     last_applied_);
  if (!s.ok() && storage_error_.ok()) storage_error_ = s;
  return s;
}

Status RaftNode::PersistLogUnlocked() {
  if (storage_ == nullptr) {
    return Status::OK();
  }
  Status s = storage_->SaveLog(log_base_index_, log_base_term_, entries_);
  if (!s.ok() && storage_error_.ok()) storage_error_ = s;
  return s;
}

Role RaftNode::role() const {
  std::lock_guard<std::mutex> lock(mu_);
  return role_;
}

Term RaftNode::current_term() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_term_;
}

NodeId RaftNode::leader_id() const {
  std::lock_guard<std::mutex> lock(mu_);
  return leader_id_;
}

NodeId RaftNode::voted_for() const {
  std::lock_guard<std::mutex> lock(mu_);
  return voted_for_;
}

LogIndex RaftNode::commit_index() const {
  std::lock_guard<std::mutex> lock(mu_);
  return commit_index_;
}

LogIndex RaftNode::last_applied() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_applied_;
}

LogIndex RaftNode::log_base_index() const {
  std::lock_guard<std::mutex> lock(mu_);
  return log_base_index_;
}

LogIndex RaftNode::LastLogIndex() const {
  std::lock_guard<std::mutex> lock(mu_);
  return LastLogIndexUnlocked();
}

Term RaftNode::LastLogTerm() const {
  std::lock_guard<std::mutex> lock(mu_);
  return TermAt(LastLogIndexUnlocked());
}

void RaftNode::SetApplyCallback(ApplyCallback cb) {
  std::lock_guard<std::mutex> lock(mu_);
  apply_cb_ = std::move(cb);
}

Status RaftNode::SetStateMachine(StateMachine* state_machine) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_machine == nullptr) {
    return Status::InvalidArgument("null state machine");
  }
  state_machine_ = state_machine;
  if (snapshot_.last_included_index != 0) {
    std::set<NodeId> old_voters;
    std::set<NodeId> new_voters;
    std::string state_data;
    Status s = DecodeSnapshotEnvelope(snapshot_.data, &old_voters,
                                      &new_voters, &state_data);
    if (!s.ok()) return s;
    s = state_machine_->RestoreSnapshot(state_data);
    if (!s.ok()) return s;
  }
  // Replaying the committed prefix makes a replaced or empty state-machine
  // store reconstructible. StateMachine::Apply is required to be idempotent.
  last_applied_ = log_base_index_;
  return MaybeApply();
}

Status RaftNode::CreateSnapshot() {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_machine_ == nullptr) {
    return Status::InvalidArgument("state machine is not installed");
  }
  if (last_applied_ <= log_base_index_) return Status::OK();
  Snapshot next;
  next.last_included_index = last_applied_;
  next.last_included_term = TermAt(last_applied_);
  std::string state_data;
  Status s = state_machine_->CreateSnapshot(&state_data);
  if (!s.ok()) return s;
  s = EncodeSnapshotEnvelope(voters_old_, voters_new_, state_data, &next.data);
  if (!s.ok()) return s;

  const size_t erase_count =
      static_cast<size_t>(next.last_included_index - log_base_index_);
  std::vector<LogEntry> suffix(entries_.begin() + erase_count, entries_.end());
  if (storage_ != nullptr) {
    s = storage_->SaveSnapshot(next);
    if (!s.ok()) {
      if (storage_error_.ok()) storage_error_ = s;
      return s;
    }
    s = storage_->SaveLog(next.last_included_index,
                          next.last_included_term, suffix);
    if (!s.ok()) {
      if (storage_error_.ok()) storage_error_ = s;
      return s;
    }
  }
  entries_ = std::move(suffix);
  log_base_index_ = next.last_included_index;
  log_base_term_ = next.last_included_term;
  snapshot_ = std::move(next);
  return PersistHardStateUnlocked();
}

std::vector<NodeId> RaftNode::voters() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::set<NodeId> all = voters_old_;
  all.insert(voters_new_.begin(), voters_new_.end());
  return std::vector<NodeId>(all.begin(), all.end());
}

Status RaftNode::ChangeMembership(const std::vector<NodeId>& voters) {
  std::set<NodeId> target(voters.begin(), voters.end());
  if (target.empty() || target.count(0) != 0) {
    return Status::InvalidArgument("membership must contain non-zero voters");
  }

  std::set<NodeId> old_voters;
  bool resume_joint = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::kLeader) return Status::InvalidArgument("not leader");
    if (!storage_error_.ok()) return storage_error_;
    old_voters = voters_old_;
    if (!voters_new_.empty()) {
      if (voters_new_ != target) {
        return Status::InvalidArgument("another joint configuration is active");
      }
      resume_joint = true;
    }
  }

  if (!resume_joint) {
    // Bring newly added replicas up to the current log before their votes are
    // required by the joint quorum.
    for (NodeId id : target) {
      if (id == config_.id || old_voters.count(id) != 0) continue;
      bool caught_up = false;
      const size_t attempts = static_cast<size_t>(LastLogIndex()) + 2;
      for (size_t i = 0; i < attempts; ++i) {
        ReplicateTo(id);
        std::lock_guard<std::mutex> lock(mu_);
        caught_up = match_index_[id] >= LastLogIndexUnlocked();
        if (caught_up) break;
      }
      if (!caught_up) return Status::IOError("new voter did not catch up");
    }

    LogIndex joint_index = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (role_ != Role::kLeader) return Status::IOError("leadership changed");
      entries_.push_back(LogEntry{
          current_term_,
          EncodeConfiguration(ConfigKind::kJoint, old_voters, target)});
      joint_index = LastLogIndexUnlocked();
      Status s = PersistLogUnlocked();
      if (!s.ok()) {
        entries_.pop_back();
        return s;
      }
      voters_new_ = target;
      for (NodeId peer : PeersUnlocked()) {
        if (next_index_.count(peer) == 0)
          next_index_[peer] = joint_index;
        if (match_index_.count(peer) == 0) match_index_[peer] = 0;
      }
    }
    ReplicateToPeers();
    {
      std::lock_guard<std::mutex> lock(mu_);
      Status s = MaybeAdvanceCommit();
      if (!s.ok()) return s;
      if (commit_index_ < joint_index) {
        return Status::IOError("joint configuration was not committed");
      }
    }
  }

  Status s = Propose(
      EncodeConfiguration(ConfigKind::kFinal, target, std::set<NodeId>()),
      nullptr);
  if (!s.ok()) return s;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!IsVoter(config_.id)) BecomeFollower(current_term_, /*leader=*/0);
  }
  return Status::OK();
}

uint64_t RaftNode::election_deadline_ms() const {
  std::lock_guard<std::mutex> lock(mu_);
  return election_deadline_ms_;
}

void RaftNode::SetElectionDeadlineForTest(uint64_t deadline_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  election_deadline_ms_ = deadline_ms;
}

void RaftNode::StepDownForTest(Term term) {
  std::lock_guard<std::mutex> lock(mu_);
  BecomeFollower(term, /*leader=*/0);
  ResetElectionDeadline(last_now_ms_);
}

bool RaftNode::GetLogEntryForTest(LogIndex index, LogEntry* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  const LogEntry* entry = EntryAt(index);
  if (entry == nullptr || out == nullptr) {
    return false;
  }
  *out = *entry;
  return true;
}

LogIndex RaftNode::MatchIndexForTest(NodeId peer) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = match_index_.find(peer);
  return it == match_index_.end() ? 0 : it->second;
}

LogIndex RaftNode::NextIndexForTest(NodeId peer) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = next_index_.find(peer);
  return it == next_index_.end() ? 0 : it->second;
}

void RaftNode::AppendLocalForTest(Term term, const std::string& command) {
  std::lock_guard<std::mutex> lock(mu_);
  entries_.push_back(LogEntry{term, command});
  PersistLogUnlocked().ok();
}

bool RaftNode::HasQuorum(const std::set<NodeId>& acknowledgements) const {
  auto has_majority = [&acknowledgements](const std::set<NodeId>& voters) {
    size_t count = 0;
    for (NodeId id : voters) {
      if (acknowledgements.count(id) != 0) ++count;
    }
    return count >= voters.size() / 2 + 1;
  };
  return has_majority(voters_old_) &&
         (voters_new_.empty() || has_majority(voters_new_));
}

bool RaftNode::IsVoter(NodeId id) const {
  return voters_old_.count(id) != 0 || voters_new_.count(id) != 0;
}

std::vector<NodeId> RaftNode::PeersUnlocked() const {
  std::set<NodeId> all = voters_old_;
  all.insert(voters_new_.begin(), voters_new_.end());
  all.erase(config_.id);
  return std::vector<NodeId>(all.begin(), all.end());
}

Status RaftNode::ApplyConfiguration(const std::string& command) {
  std::set<NodeId> old_voters;
  std::set<NodeId> new_voters;
  const ConfigKind kind =
      DecodeConfiguration(command, &old_voters, &new_voters);
  if (kind == ConfigKind::kNone) return Status::NotFound("not configuration");
  if (kind == ConfigKind::kJoint) {
    if (new_voters.empty()) return Status::Corruption("empty new voter set");
    voters_old_ = std::move(old_voters);
    voters_new_ = std::move(new_voters);
  } else {
    voters_old_ = std::move(old_voters);
    voters_new_.clear();
  }
  return Status::OK();
}

Status RaftNode::RebuildConfiguration() {
  voters_old_ = initial_voters_;
  voters_new_.clear();
  if (snapshot_.last_included_index != 0) {
    std::string state_data;
    Status s = DecodeSnapshotEnvelope(snapshot_.data, &voters_old_,
                                      &voters_new_, &state_data);
    if (!s.ok()) return s;
  }
  for (LogIndex i = log_base_index_ + 1; i <= LastLogIndexUnlocked(); ++i) {
    const LogEntry* entry = EntryAt(i);
    if (entry == nullptr) return Status::Corruption("missing config log entry");
    std::set<NodeId> old_voters;
    std::set<NodeId> new_voters;
    ConfigKind kind =
        DecodeConfiguration(entry->command, &old_voters, &new_voters);
    if (kind == ConfigKind::kNone) continue;
    if (i <= commit_index_ || kind == ConfigKind::kJoint) {
      Status s = ApplyConfiguration(entry->command);
      if (!s.ok()) return s;
    }
  }
  return Status::OK();
}

Term RaftNode::TermAt(LogIndex index) const {
  if (index == 0) {
    return 0;
  }
  if (index == log_base_index_) return log_base_term_;
  const LogEntry* entry = EntryAt(index);
  return entry == nullptr ? 0 : entry->term;
}

const LogEntry* RaftNode::EntryAt(LogIndex index) const {
  if (index <= log_base_index_ || index > LastLogIndexUnlocked()) {
    return nullptr;
  }
  return &entries_[static_cast<size_t>(index - log_base_index_ - 1)];
}

LogIndex RaftNode::LastLogIndexUnlocked() const {
  return log_base_index_ + static_cast<LogIndex>(entries_.size());
}

bool RaftNode::IsLogUpToDate(LogIndex cand_last_index, Term cand_last_term) const {
  const LogIndex my_index = LastLogIndexUnlocked();
  const Term my_term = TermAt(my_index);
  if (cand_last_term != my_term) {
    return cand_last_term > my_term;
  }
  return cand_last_index >= my_index;
}

void RaftNode::ResetElectionDeadline(uint64_t now_ms) {
  uint64_t base = now_ms;
  if (base == 0 && last_now_ms_ != 0) {
    base = last_now_ms_;
  }
  if (base == 0) {
    base = election_deadline_ms_;
  }
  const int lo = config_.election_timeout_min_ms;
  const int hi = std::max(lo, config_.election_timeout_max_ms);
  const int span = hi - lo + 1;
  const int jitter =
      static_cast<int>(NextRand(&rng_state_) % static_cast<uint64_t>(span));
  election_deadline_ms_ = base + static_cast<uint64_t>(lo + jitter);
}

void RaftNode::BecomeFollower(Term term, NodeId leader) {
  bool dirty = false;
  if (term > current_term_) {
    current_term_ = term;
    voted_for_ = 0;
    dirty = true;
  } else {
    current_term_ = term;
    // A node must never clear its vote within the same term.
  }
  role_ = Role::kFollower;
  leader_id_ = leader;
  votes_granted_ = 0;
  if (dirty) {
    PersistHardStateUnlocked().ok();
  }
}

void RaftNode::BecomeCandidate() {
  role_ = Role::kCandidate;
  ++current_term_;
  voted_for_ = config_.id;
  leader_id_ = 0;
  votes_granted_ = 1;
  PersistHardStateUnlocked().ok();
}

void RaftNode::BecomeLeader() {
  role_ = Role::kLeader;
  leader_id_ = config_.id;
  next_heartbeat_ms_ = 0;
  const LogIndex last = LastLogIndexUnlocked();
  next_index_.clear();
  match_index_.clear();
  for (NodeId p : PeersUnlocked()) {
    next_index_[p] = last + 1;
    match_index_[p] = 0;
  }
}

Status RaftNode::MaybeApply() {
  while (last_applied_ < commit_index_) {
    const LogIndex next = last_applied_ + 1;
    const LogEntry* entry = EntryAt(next);
    if (entry == nullptr) return Status::Corruption("missing committed entry");
    const LogEntry& e = *entry;
    Status configuration = ApplyConfiguration(e.command);
    if (configuration.ok()) {
      if (role_ == Role::kLeader) {
        for (NodeId peer : PeersUnlocked()) {
          if (next_index_.count(peer) == 0)
            next_index_[peer] = LastLogIndexUnlocked() + 1;
          if (match_index_.count(peer) == 0) match_index_[peer] = 0;
        }
      }
    } else if (!configuration.IsNotFound()) {
      return configuration;
    } else if (state_machine_ != nullptr) {
      Status s = state_machine_->Apply(next, e.term, e.command);
      if (!s.ok()) return s;
    } else if (apply_cb_) {
      apply_cb_(next, e.term, e.command);
    }
    last_applied_ = next;
    Status s = PersistHardStateUnlocked();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

Status RaftNode::MaybeAdvanceCommit() {
  // A previous Apply may have failed after commit_index_ advanced. Always
  // finish that committed prefix before considering newer entries.
  if (last_applied_ < commit_index_) {
    Status s = MaybeApply();
    if (!s.ok()) return s;
  }
  // 只能提交当前任期已复制到多数派的条目（Raft Figure 8）。
  for (LogIndex n = LastLogIndexUnlocked(); n > commit_index_;
       --n) {
    if (TermAt(n) != current_term_) {
      continue;
    }
    std::set<NodeId> acknowledgements{config_.id};
    for (NodeId p : PeersUnlocked()) {
      if (match_index_[p] >= n) {
        acknowledgements.insert(p);
      }
    }
    if (HasQuorum(acknowledgements)) {
      commit_index_ = n;
      return MaybeApply();
    }
  }
  return Status::OK();
}

Status RaftNode::Propose(const std::string& command, LogIndex* index) {
  LogIndex proposed_index = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::kLeader) {
      return Status::InvalidArgument("not leader");
    }
    if (!storage_error_.ok()) return storage_error_;
    entries_.push_back(LogEntry{current_term_, command});
    proposed_index = LastLogIndexUnlocked();
    if (index != nullptr) {
      *index = proposed_index;
    }
    Status persist = PersistLogUnlocked();
    if (!persist.ok()) {
      entries_.pop_back();
      return persist;
    }
  }
  ReplicateToPeers();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ == Role::kLeader) {
      Status s = MaybeAdvanceCommit();
      if (!s.ok()) return s;
      if (commit_index_ < proposed_index) {
        return Status::IOError("proposal was not committed by a majority");
      }
    } else {
      return Status::IOError("leadership changed before proposal committed");
    }
  }
  return Status::OK();
}

Status RaftNode::ConfirmLeadership() {
  std::vector<NodeId> peers;
  Term term = 0;
  LogIndex barrier_index = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::kLeader) return Status::InvalidArgument("not leader");
    if (!storage_error_.ok()) return storage_error_;
    // A quorum heartbeat alone does not make entries from older terms
    // committable. Add an internal no-op once per leadership term so a read
    // after election is ordered behind every previously committed entry.
    if (TermAt(LastLogIndexUnlocked()) != current_term_) {
      entries_.push_back(LogEntry{current_term_, std::string()});
      Status persist = PersistLogUnlocked();
      if (!persist.ok()) {
        entries_.pop_back();
        return persist;
      }
    }
    barrier_index = LastLogIndexUnlocked();
    peers = PeersUnlocked();
    term = current_term_;
  }
  std::set<NodeId> confirmed{config_.id};
  for (NodeId peer : peers) {
    if (ReplicateTo(peer)) confirmed.insert(peer);
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::kLeader || current_term_ != term) {
      return Status::IOError("leadership changed");
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!HasQuorum(confirmed)) {
      return Status::IOError("leadership was not confirmed by a quorum");
    }
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (role_ != Role::kLeader || current_term_ != term) {
    return Status::IOError("leadership changed");
  }
  Status s = MaybeAdvanceCommit();
  if (!s.ok()) return s;
  if (commit_index_ < barrier_index) {
    return Status::IOError("read barrier was not committed by a majority");
  }
  return MaybeApply();
}

void RaftNode::Tick(uint64_t now_ms) {
  std::unique_lock<std::mutex> lock(mu_);
  last_now_ms_ = now_ms;
  if (role_ == Role::kLeader) {
    if (now_ms >= next_heartbeat_ms_) {
      lock.unlock();
      ReplicateToPeers();
      lock.lock();
      if (role_ == Role::kLeader) {
        next_heartbeat_ms_ =
            now_ms + static_cast<uint64_t>(config_.heartbeat_interval_ms);
      }
    }
    return;
  }

  if (now_ms >= election_deadline_ms_) {
    lock.unlock();
    StartElection(now_ms);
  }
}

void RaftNode::StartElection(uint64_t now_ms) {
  rpc::RaftRequestVoteReq req;
  std::vector<NodeId> peers;
  Term my_term = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!IsVoter(config_.id)) return;
    BecomeCandidate();
    if (!storage_error_.ok()) return;
    ResetElectionDeadline(now_ms);
    my_term = current_term_;
    req.term = current_term_;
    req.candidate_id = config_.id;
    req.last_log_index = LastLogIndexUnlocked();
    req.last_log_term = TermAt(req.last_log_index);
    peers = PeersUnlocked();
  }

  std::set<NodeId> granted{config_.id};
  for (NodeId peer : peers) {
    rpc::RaftRequestVoteResp resp;
    Status s = transport_->SendRequestVote(peer, req, &resp);
    if (!s.ok()) {
      continue;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (resp.term > current_term_) {
      BecomeFollower(resp.term, /*leader=*/0);
      ResetElectionDeadline(now_ms);
      return;
    }
    if (role_ != Role::kCandidate || current_term_ != my_term) {
      return;
    }
    if (resp.vote_granted) {
      granted.insert(peer);
      votes_granted_ = static_cast<int>(granted.size());
      if (HasQuorum(granted)) {
        BecomeLeader();
        return;
      }
    }
  }
  // 单节点（无 peer）时 for 循环不跑，仍需在本地多数派下成主。
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ == Role::kCandidate && current_term_ == my_term &&
        HasQuorum(granted)) {
      BecomeLeader();
    }
  }
}

void RaftNode::ReplicateToPeers() {
  std::vector<NodeId> peers;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::kLeader) {
      return;
    }
    peers = PeersUnlocked();
  }
  for (NodeId peer : peers) {
    ReplicateTo(peer);
  }
}

bool RaftNode::ReplicateTo(NodeId peer) {
  rpc::RaftAppendEntriesReq req;
  rpc::RaftInstallSnapshotReq snapshot_req;
  bool install_snapshot = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::kLeader) {
      return false;
    }
    LogIndex next = next_index_[peer];
    if (next == 0) {
      next = log_base_index_ + 1;
      next_index_[peer] = next;
    }
    if (next <= log_base_index_) {
      install_snapshot = true;
      snapshot_req.term = current_term_;
      snapshot_req.leader_id = config_.id;
      snapshot_req.last_included_index = snapshot_.last_included_index;
      snapshot_req.last_included_term = snapshot_.last_included_term;
      snapshot_req.data = snapshot_.data;
    } else {
      req.term = current_term_;
      req.leader_id = config_.id;
      req.prev_log_index = next - 1;
      req.prev_log_term = TermAt(req.prev_log_index);
      req.leader_commit = commit_index_;
      req.entries.clear();
      for (LogIndex i = next; i <= LastLogIndexUnlocked(); ++i) {
        const LogEntry& e = *EntryAt(i);
        rpc::RaftLogEntry re;
        re.term = e.term;
        re.command = e.command;
        req.entries.push_back(std::move(re));
      }
    }
  }

  if (install_snapshot) {
    rpc::RaftInstallSnapshotResp snapshot_resp;
    Status s = transport_->SendInstallSnapshot(peer, snapshot_req, &snapshot_resp);
    if (!s.ok()) return false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (snapshot_resp.term > current_term_) {
        BecomeFollower(snapshot_resp.term, /*leader=*/0);
        ResetElectionDeadline(last_now_ms_);
        return false;
      }
      if (role_ != Role::kLeader || !snapshot_resp.success) return false;
      match_index_[peer] = snapshot_req.last_included_index;
      next_index_[peer] = snapshot_req.last_included_index + 1;
    }
    return ReplicateTo(peer);
  }

  rpc::RaftAppendEntriesResp resp;
  Status s = transport_->SendAppendEntries(peer, req, &resp);
  if (!s.ok()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (resp.term > current_term_) {
    BecomeFollower(resp.term, /*leader=*/0);
    ResetElectionDeadline(last_now_ms_);
    return false;
  }
  if (role_ != Role::kLeader) {
    return false;
  }
  if (resp.success) {
    const LogIndex matched =
        req.prev_log_index + static_cast<LogIndex>(req.entries.size());
    match_index_[peer] = matched;
    next_index_[peer] = matched + 1;
    MaybeAdvanceCommit().ok();
    return true;
  } else {
    if (next_index_[peer] > 1) {
      --next_index_[peer];
    }
  }
  return false;
}

rpc::RaftRequestVoteResp RaftNode::OnRequestVote(const rpc::RaftRequestVoteReq& req) {
  std::lock_guard<std::mutex> lock(mu_);
  rpc::RaftRequestVoteResp resp;
  resp.vote_granted = 0;

  if (req.term < current_term_) {
    resp.term = current_term_;
    return resp;
  }
  if (req.term > current_term_) {
    BecomeFollower(req.term, /*leader=*/0);
    if (!storage_error_.ok()) {
      resp.term = current_term_;
      return resp;
    }
  }

  const bool can_vote =
      IsVoter(config_.id) && IsVoter(req.candidate_id) &&
      (voted_for_ == 0 || voted_for_ == req.candidate_id) &&
      IsLogUpToDate(req.last_log_index, req.last_log_term);
  if (can_vote) {
    voted_for_ = req.candidate_id;
    role_ = Role::kFollower;
    leader_id_ = 0;
    resp.vote_granted = 1;
    ResetElectionDeadline(last_now_ms_);
    if (!PersistHardStateUnlocked().ok()) resp.vote_granted = 0;
  }
  resp.term = current_term_;
  return resp;
}

rpc::RaftAppendEntriesResp RaftNode::OnAppendEntries(
    const rpc::RaftAppendEntriesReq& req) {
  std::lock_guard<std::mutex> lock(mu_);
  rpc::RaftAppendEntriesResp resp;
  resp.success = 0;

  if (req.term < current_term_) {
    resp.term = current_term_;
    return resp;
  }

  if (req.term > current_term_) {
    BecomeFollower(req.term, req.leader_id);
    if (!storage_error_.ok()) {
      resp.term = current_term_;
      return resp;
    }
  } else {
    role_ = Role::kFollower;
    leader_id_ = req.leader_id;
    current_term_ = req.term;
  }
  // Once durable state has failed, this process must not acknowledge later
  // requests from memory alone. A restart is required to reload a known-good
  // durable prefix.
  if (!storage_error_.ok()) {
    resp.term = current_term_;
    return resp;
  }
  ResetElectionDeadline(last_now_ms_);

  // prevLog 一致性检查。
  if (req.prev_log_index < log_base_index_ ||
      req.prev_log_index > LastLogIndexUnlocked()) {
    resp.term = current_term_;
    return resp;
  }
  if (req.prev_log_index > 0 &&
      TermAt(req.prev_log_index) != req.prev_log_term) {
    resp.term = current_term_;
    return resp;
  }

  // 冲突则截断，再追加。
  const std::vector<LogEntry> old_entries = entries_;
  LogIndex idx = req.prev_log_index;
  bool log_changed = false;
  for (size_t i = 0; i < req.entries.size(); ++i) {
    ++idx;
    const auto& incoming = req.entries[i];
    if (idx <= LastLogIndexUnlocked()) {
      if (TermAt(idx) != incoming.term) {
        entries_.resize(static_cast<size_t>(idx - log_base_index_ - 1));
        entries_.push_back(LogEntry{incoming.term, incoming.command});
        log_changed = true;
      }
    } else {
      entries_.push_back(LogEntry{incoming.term, incoming.command});
      log_changed = true;
    }
  }
  if (log_changed) {
    if (!PersistLogUnlocked().ok()) {
      entries_ = old_entries;
      resp.term = current_term_;
      return resp;
    }
    Status config_status = RebuildConfiguration();
    if (!config_status.ok()) {
      resp.term = current_term_;
      return resp;
    }
  }

  if (req.leader_commit > commit_index_) {
    commit_index_ =
        std::min(req.leader_commit, LastLogIndexUnlocked());
  }
  // Apply can fail independently of log persistence. Retry an unapplied
  // committed suffix on every heartbeat instead of acknowledging it as done.
  if (last_applied_ < commit_index_) {
    Status apply = MaybeApply();
    if (!apply.ok()) {
      resp.term = current_term_;
      return resp;
    }
  }

  resp.term = current_term_;
  resp.success = 1;
  return resp;
}

rpc::RaftInstallSnapshotResp RaftNode::OnInstallSnapshot(
    const rpc::RaftInstallSnapshotReq& req) {
  std::lock_guard<std::mutex> lock(mu_);
  rpc::RaftInstallSnapshotResp resp;
  resp.success = 0;
  if (req.term < current_term_) {
    resp.term = current_term_;
    return resp;
  }
  if (req.term > current_term_) {
    BecomeFollower(req.term, req.leader_id);
  } else {
    role_ = Role::kFollower;
    leader_id_ = req.leader_id;
  }
  resp.term = current_term_;
  if (!storage_error_.ok()) return resp;
  ResetElectionDeadline(last_now_ms_);
  if (req.last_included_index <= log_base_index_) {
    resp.success = 1;
    return resp;
  }

  Snapshot next;
  next.last_included_index = req.last_included_index;
  next.last_included_term = req.last_included_term;
  next.data = req.data;
  std::set<NodeId> snapshot_old;
  std::set<NodeId> snapshot_new;
  std::string state_data;
  Status s = DecodeSnapshotEnvelope(next.data, &snapshot_old, &snapshot_new,
                                    &state_data);
  if (!s.ok()) return resp;

  std::vector<LogEntry> suffix;
  if (req.last_included_index <= LastLogIndexUnlocked() &&
      TermAt(req.last_included_index) == req.last_included_term) {
    const size_t first = static_cast<size_t>(
        req.last_included_index - log_base_index_);
    suffix.assign(entries_.begin() + first, entries_.end());
  }

  s = Status::OK();
  if (storage_ != nullptr) {
    s = storage_->SaveSnapshot(next);
    if (s.ok()) {
      s = storage_->SaveLog(next.last_included_index,
                            next.last_included_term, suffix);
    }
    if (!s.ok()) {
      if (storage_error_.ok()) storage_error_ = s;
      return resp;
    }
  }
  if (state_machine_ != nullptr) {
    s = state_machine_->RestoreSnapshot(state_data);
    if (!s.ok()) return resp;
  }

  entries_ = std::move(suffix);
  log_base_index_ = next.last_included_index;
  log_base_term_ = next.last_included_term;
  snapshot_ = std::move(next);
  voters_old_ = std::move(snapshot_old);
  voters_new_ = std::move(snapshot_new);
  commit_index_ = std::max(commit_index_, log_base_index_);
  last_applied_ = std::max(last_applied_, log_base_index_);
  s = RebuildConfiguration();
  if (!s.ok()) return resp;
  s = PersistHardStateUnlocked();
  if (!s.ok()) return resp;
  resp.success = 1;
  return resp;
}

}  // namespace raft
}  // namespace minikv
