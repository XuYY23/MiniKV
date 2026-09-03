#include "env.h"
#include "minikv/db.h"
#include "raft/memory_transport.h"
#include "raft/node.h"
#include "raft/state_machine.h"
#include "raft/storage.h"
#include "tests/testharness.h"

#include <string>
#include <vector>

namespace {

class FailingStorage : public minikv::raft::RaftStorageInterface {
 public:
  bool fail_hard = false;
  bool fail_log = false;
  minikv::Status Load(minikv::raft::Term* term,
                      minikv::raft::NodeId* voted_for,
                      minikv::raft::LogIndex* log_base_index,
                      minikv::raft::Term* log_base_term,
                      std::vector<minikv::raft::LogEntry>* entries,
                      minikv::raft::LogIndex* commit,
                      minikv::raft::LogIndex* applied,
                      minikv::raft::Snapshot* snapshot) override {
    *term = 0;
    *voted_for = 0;
    entries->clear();
    *commit = 0;
    *applied = 0;
    *log_base_index = 0;
    *log_base_term = 0;
    *snapshot = minikv::raft::Snapshot();
    return minikv::Status::OK();
  }
  minikv::Status SaveHardState(minikv::raft::Term, minikv::raft::NodeId,
                               minikv::raft::LogIndex,
                               minikv::raft::LogIndex) override {
    return fail_hard ? minikv::Status::IOError("injected hard-state failure")
                     : minikv::Status::OK();
  }
  minikv::Status SaveLog(minikv::raft::LogIndex,
      minikv::raft::Term,
      const std::vector<minikv::raft::LogEntry>&) override {
    return fail_log ? minikv::Status::IOError("injected log failure")
                    : minikv::Status::OK();
  }
  minikv::Status SaveSnapshot(const minikv::raft::Snapshot&) override {
    return minikv::Status::OK();
  }
};

class FailingStateMachine : public minikv::raft::StateMachine {
 public:
  bool fail = true;
  int calls = 0;
  minikv::Status Apply(minikv::raft::LogIndex, minikv::raft::Term,
                       const std::string&) override {
    ++calls;
    return fail ? minikv::Status::IOError("injected apply failure")
                : minikv::Status::OK();
  }
};

minikv::raft::RaftConfig MakeSolo(minikv::raft::NodeId id) {
  minikv::raft::RaftConfig c;
  c.id = id;
  c.election_timeout_min_ms = 150;
  c.election_timeout_max_ms = 150;
  c.heartbeat_interval_ms = 40;
  return c;
}

void CleanupRaftDir(const std::string& dir) {
  minikv::test::RemoveFileIfExists(dir + "/hard_state");
  minikv::test::RemoveFileIfExists(dir + "/hard_state.tmp");
  minikv::test::RemoveFileIfExists(dir + "/raft.log");
  minikv::test::RemoveFileIfExists(dir + "/raft.log.tmp");
  minikv::test::RemoveFileIfExists(dir + "/snapshot");
  minikv::test::RemoveFileIfExists(dir + "/snapshot.tmp");
}

void CleanupDbDir(const std::string& dbname) {
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST");
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST.tmp");
  for (int i = 1; i < 64; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%06d.log", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
  }
}

}  // namespace

int main() {
  minikv::test::LogSection(
      "TEST SUITE: test_raft_persist (小节 D4 · 持久化 + Apply 接引擎)");

  minikv::test::LogStep("persistence failures are never acknowledged");
  {
    FailingStorage storage;
    storage.fail_hard = true;
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode node(MakeSolo(1), &net, &storage);
    CHECK_OK(node.InitStorage());
    minikv::rpc::RaftRequestVoteReq vote;
    vote.term = 1;
    vote.candidate_id = 2;
    auto response = node.OnRequestVote(vote);
    CHECK_EQ(static_cast<int>(response.vote_granted), 0);
  }
  {
    FailingStorage storage;
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode node(MakeSolo(1), &net, &storage);
    CHECK_OK(node.InitStorage());
    node.SetElectionDeadlineForTest(100);
    node.Tick(100);
    CHECK(node.role() == minikv::raft::Role::kLeader);
    storage.fail_log = true;
    CHECK(!node.Propose("must-fail", nullptr).ok());
    CHECK_EQ(node.LastLogIndex(), static_cast<uint64_t>(0));
  }
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode node(MakeSolo(1), &net);
    FailingStateMachine state_machine;
    CHECK_OK(node.SetStateMachine(&state_machine));
    node.SetElectionDeadlineForTest(100);
    node.Tick(100);
    CHECK(!node.Propose("apply-must-fail", nullptr).ok());
    CHECK_EQ(node.commit_index(), static_cast<uint64_t>(1));
    CHECK_EQ(node.last_applied(), static_cast<uint64_t>(0));
    state_machine.fail = false;
    CHECK_OK(node.ConfirmLeadership());
    CHECK_EQ(node.last_applied(), static_cast<uint64_t>(1));
  }
  {
    FailingStorage storage;
    storage.fail_log = true;
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode node(MakeSolo(1), &net, &storage);
    CHECK_OK(node.InitStorage());
    minikv::rpc::RaftAppendEntriesReq append;
    append.term = 1;
    append.leader_id = 2;
    append.entries.push_back({1, "not-durable"});
    auto response = node.OnAppendEntries(append);
    CHECK_EQ(static_cast<int>(response.success), 0);
    CHECK_EQ(node.LastLogIndex(), static_cast<uint64_t>(0));
    storage.fail_log = false;
    response = node.OnAppendEntries(append);
    CHECK_EQ(static_cast<int>(response.success), 0);
  }
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode node(MakeSolo(1), &net);
    FailingStateMachine state_machine;
    CHECK_OK(node.SetStateMachine(&state_machine));
    minikv::rpc::RaftAppendEntriesReq append;
    append.term = 1;
    append.leader_id = 2;
    append.leader_commit = 1;
    append.entries.push_back({1, "retry-apply"});
    auto response = node.OnAppendEntries(append);
    CHECK_EQ(static_cast<int>(response.success), 0);
    CHECK_EQ(node.commit_index(), static_cast<uint64_t>(1));
    CHECK_EQ(node.last_applied(), static_cast<uint64_t>(0));
    state_machine.fail = false;
    minikv::rpc::RaftAppendEntriesReq heartbeat;
    heartbeat.term = 1;
    heartbeat.leader_id = 2;
    heartbeat.prev_log_index = 1;
    heartbeat.prev_log_term = 1;
    heartbeat.leader_commit = 1;
    response = node.OnAppendEntries(heartbeat);
    CHECK_EQ(static_cast<int>(response.success), 1);
    CHECK_EQ(node.last_applied(), static_cast<uint64_t>(1));
    CHECK_EQ(state_machine.calls, 2);
  }

  const std::string root = minikv::test::MakeTestDir("raft_persist");
  const std::string raft_dir = root + "/raft1";
  const std::string db_dir = root + "/kv";
  CleanupRaftDir(raft_dir);
  CleanupDbDir(db_dir);
  if (!minikv::Env::Default()->FileExists(raft_dir)) {
    CHECK_OK(minikv::Env::Default()->CreateDir(raft_dir));
  }
  if (!minikv::Env::Default()->FileExists(db_dir)) {
    CHECK_OK(minikv::Env::Default()->CreateDir(db_dir));
  }

  // ---------- basic ----------
  minikv::test::LogStep("basic: persist hard_state+log across RaftNode restart");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftStorage storage(raft_dir);
    minikv::raft::RaftNode n1(MakeSolo(1), &net, &storage);
    net.Register(&n1);
    CHECK_OK(n1.InitStorage());
    n1.SetElectionDeadlineForTest(100);
    n1.Tick(100);
    CHECK(n1.role() == minikv::raft::Role::kLeader);

    minikv::raft::LogIndex idx = 0;
    CHECK_OK(n1.Propose(minikv::raft::KvCommand::EncodePut("k1", "v1"), &idx));
    CHECK_OK(n1.Propose(minikv::raft::KvCommand::EncodePut("k2", "v2"), &idx));
    CHECK_EQ(n1.commit_index(), static_cast<uint64_t>(2));
    CHECK_EQ(n1.current_term(), static_cast<uint64_t>(1));

    net.Unregister(1);
  }
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftStorage storage(raft_dir);
    minikv::raft::RaftNode n1(MakeSolo(1), &net, &storage);
    net.Register(&n1);
    CHECK_OK(n1.InitStorage());
    CHECK_EQ(n1.current_term(), static_cast<uint64_t>(1));
    CHECK_EQ(n1.voted_for(), static_cast<uint64_t>(1));
    CHECK_EQ(n1.LastLogIndex(), static_cast<uint64_t>(2));
    CHECK_EQ(n1.commit_index(), static_cast<uint64_t>(2));
    minikv::raft::LogEntry e;
    CHECK(n1.GetLogEntryForTest(1, &e));
    CHECK_EQ(e.command, minikv::raft::KvCommand::EncodePut("k1", "v1"));
    minikv::test::Log("  recovered term/vote/log/commit");
  }

  minikv::test::LogStep("committed log reconstructs an empty state machine");
  {
    CleanupDbDir(db_dir);
    minikv::DB* db = nullptr;
    CHECK_OK(minikv::DB::Open(minikv::Options(), db_dir, &db));
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftStorage storage(raft_dir);
    minikv::raft::RaftNode node(MakeSolo(1), &net, &storage);
    CHECK_OK(node.InitStorage());
    minikv::raft::KvStateMachine state_machine(db);
    CHECK_OK(node.SetStateMachine(&state_machine));
    std::string value;
    CHECK_OK(db->Get(minikv::ReadOptions(), "k1", &value));
    CHECK_EQ(value, std::string("v1"));
    CHECK_OK(db->Get(minikv::ReadOptions(), "k2", &value));
    CHECK_EQ(value, std::string("v2"));
    CHECK_OK(db->Close());
    delete db;
  }

  minikv::test::LogStep("basic: Apply callback writes into MiniKV DB");
  {
    CleanupDbDir(db_dir);
    minikv::Options options;
    options.create_if_missing = true;
    options.sync = false;
    minikv::DB* db = nullptr;
    CHECK_OK(minikv::DB::Open(options, db_dir, &db));

    minikv::raft::MemoryTransport net;
    minikv::raft::RaftStorage storage(root + "/raft_apply");
    CleanupRaftDir(root + "/raft_apply");
    minikv::raft::RaftNode n1(MakeSolo(1), &net, &storage);
    net.Register(&n1);
    CHECK_OK(n1.InitStorage());

    minikv::raft::KvStateMachine sm(db);
    n1.SetApplyCallback([&](minikv::raft::LogIndex idx, minikv::raft::Term term,
                            const std::string& cmd) {
      CHECK_OK_QUIET(sm.Apply(idx, term, cmd));
    });

    n1.SetElectionDeadlineForTest(100);
    n1.Tick(100);
    CHECK_OK(n1.Propose(minikv::raft::KvCommand::EncodePut("user", "alice"),
                        nullptr));
    CHECK_OK(n1.Propose(minikv::raft::KvCommand::EncodeDelete("user"), nullptr));
    CHECK_OK(n1.Propose(minikv::raft::KvCommand::EncodePut("user", "bob"),
                        nullptr));

    std::string value;
    minikv::ReadOptions ro;
    CHECK_OK(db->Get(ro, "user", &value));
    CHECK_EQ(value, std::string("bob"));
    CHECK_OK(db->Close());
    delete db;
    minikv::test::Log("  LSM reflects Apply order: final user=bob");
  }

  // ---------- complex ----------
  minikv::test::LogStep(
      "complex: recover after crash mid-term; re-Apply must be idempotent");
  {
    const std::string rdir = root + "/raft_crash";
    const std::string kdir = root + "/kv_crash";
    CleanupRaftDir(rdir);
    CleanupDbDir(kdir);
    if (!minikv::Env::Default()->FileExists(rdir)) {
      CHECK_OK(minikv::Env::Default()->CreateDir(rdir));
    }
    if (!minikv::Env::Default()->FileExists(kdir)) {
      CHECK_OK(minikv::Env::Default()->CreateDir(kdir));
    }

    minikv::Options options;
    options.create_if_missing = true;
    options.sync = false;
    minikv::DB* db = nullptr;
    CHECK_OK(minikv::DB::Open(options, kdir, &db));

    {
      minikv::raft::MemoryTransport net;
      minikv::raft::RaftStorage storage(rdir);
      minikv::raft::RaftNode n1(MakeSolo(1), &net, &storage);
      net.Register(&n1);
      CHECK_OK(n1.InitStorage());
      minikv::raft::KvStateMachine sm(db);
      n1.SetApplyCallback([&](minikv::raft::LogIndex i, minikv::raft::Term t,
                              const std::string& c) {
        CHECK_OK_QUIET(sm.Apply(i, t, c));
      });
      n1.SetElectionDeadlineForTest(100);
      n1.Tick(100);
      CHECK_OK(n1.Propose(minikv::raft::KvCommand::EncodePut("x", "1"), nullptr));
      CHECK_EQ(n1.last_applied(), static_cast<uint64_t>(1));
    }

    // 「崩溃」后：DB 已有 x=1；Raft 恢复 last_applied=1，不应重复打乱。
    CHECK_OK(db->Close());
    delete db;
    db = nullptr;
    CHECK_OK(minikv::DB::Open(options, kdir, &db));

    {
      minikv::raft::MemoryTransport net;
      minikv::raft::RaftStorage storage(rdir);
      minikv::raft::RaftNode n1(MakeSolo(1), &net, &storage);
      net.Register(&n1);
      CHECK_OK(n1.InitStorage());
      CHECK_EQ(n1.last_applied(), static_cast<uint64_t>(1));
      CHECK_EQ(n1.commit_index(), static_cast<uint64_t>(1));

      minikv::raft::KvStateMachine sm(db);
      n1.SetApplyCallback([&](minikv::raft::LogIndex i, minikv::raft::Term t,
                              const std::string& c) {
        CHECK_OK_QUIET(sm.Apply(i, t, c));
      });
      n1.SetElectionDeadlineForTest(100);
      n1.Tick(100);
      CHECK_OK(n1.Propose(minikv::raft::KvCommand::EncodePut("x", "2"), nullptr));
      CHECK_EQ(n1.last_applied(), static_cast<uint64_t>(2));
    }

    std::string value;
    CHECK_OK(db->Get(minikv::ReadOptions(), "x", &value));
    CHECK_EQ(value, std::string("2"));
    CHECK_OK(db->Close());
    delete db;
    minikv::test::Log("  crash recover: last_applied preserved; new Propose OK");
  }

  minikv::test::LogStep(
      "complex: corrupted hard_state rejected; empty dir still boots");
  {
    const std::string bad = root + "/raft_bad";
    CleanupRaftDir(bad);
    if (!minikv::Env::Default()->FileExists(bad)) {
      CHECK_OK(minikv::Env::Default()->CreateDir(bad));
    }
    // 写坏 magic
    {
      std::unique_ptr<minikv::WritableFile> f;
      CHECK_OK(minikv::Env::Default()->NewWritableFile(bad + "/hard_state", &f));
      CHECK_OK(f->Append("XXXX"));
      CHECK_OK(f->Close());
    }
    minikv::raft::RaftStorage storage(bad);
    minikv::raft::Term term = 0;
    minikv::raft::NodeId voted = 0;
    std::vector<minikv::raft::LogEntry> entries;
    minikv::raft::LogIndex c = 0, a = 0;
    minikv::raft::LogIndex base = 0;
    minikv::raft::Term base_term = 0;
    minikv::raft::Snapshot snapshot;
    CHECK(!storage.Load(&term, &voted, &base, &base_term, &entries, &c, &a,
                        &snapshot).ok());

    CleanupRaftDir(bad);
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode n1(MakeSolo(1), &net, &storage);
    CHECK_OK(n1.InitStorage());
    CHECK_EQ(n1.current_term(), static_cast<uint64_t>(0));
    minikv::test::Log("  bad magic fails Load; clean dir loads empty OK");
  }

  return minikv::test::Report("test_raft_persist");
}
