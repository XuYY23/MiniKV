#include "raft/memory_transport.h"
#include "raft/node.h"
#include "tests/testharness.h"

#include <string>
#include <vector>

namespace {

minikv::raft::RaftConfig MakeConfig(minikv::raft::NodeId id,
                                    const std::vector<minikv::raft::NodeId>& peers) {
  minikv::raft::RaftConfig c;
  c.id = id;
  c.peers = peers;
  c.election_timeout_min_ms = 150;
  c.election_timeout_max_ms = 150;
  c.heartbeat_interval_ms = 40;
  return c;
}

void ElectLeader1(minikv::raft::RaftNode* n1, minikv::raft::RaftNode* n2,
                  minikv::raft::RaftNode* n3) {
  n1->SetElectionDeadlineForTest(100);
  n2->SetElectionDeadlineForTest(5000);
  n3->SetElectionDeadlineForTest(5000);
  n1->Tick(100);
  CHECK(n1->role() == minikv::raft::Role::kLeader);
  n1->Tick(150);
  n2->Tick(150);
  n3->Tick(150);
  CHECK_EQ(n2->leader_id(), static_cast<uint64_t>(1));
  CHECK_EQ(n3->leader_id(), static_cast<uint64_t>(1));
}

}  // namespace

int main() {
  minikv::test::LogSection(
      "TEST SUITE: test_raft_replicate (小节 D3 · 日志复制与提交)");

  // ---------- 基础路径 ----------
  minikv::test::LogStep("basic: Leader Propose -> majority commit -> Apply");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode n1(MakeConfig(1, {2, 3}), &net);
    minikv::raft::RaftNode n2(MakeConfig(2, {1, 3}), &net);
    minikv::raft::RaftNode n3(MakeConfig(3, {1, 2}), &net);
    net.Register(&n1);
    net.Register(&n2);
    net.Register(&n3);

    std::vector<std::string> applied;
    n1.SetApplyCallback([&](minikv::raft::LogIndex, minikv::raft::Term,
                            const std::string& cmd) { applied.push_back(cmd); });

    ElectLeader1(&n1, &n2, &n3);

    minikv::raft::LogIndex idx = 0;
    CHECK_OK(n1.Propose("put a=1", &idx));
    CHECK_EQ(idx, static_cast<uint64_t>(1));
    CHECK_OK(n1.Propose("put b=2", &idx));
    CHECK_EQ(idx, static_cast<uint64_t>(2));

    n1.Tick(200);
    n2.Tick(200);
    n3.Tick(200);

    CHECK_EQ(n1.commit_index(), static_cast<uint64_t>(2));
    CHECK_EQ(n2.commit_index(), static_cast<uint64_t>(2));
    CHECK_EQ(n3.commit_index(), static_cast<uint64_t>(2));
    CHECK_EQ(n1.last_applied(), static_cast<uint64_t>(2));
    CHECK_EQ(static_cast<int>(applied.size()), 2);
    CHECK_EQ(applied[0], std::string("put a=1"));
    CHECK_EQ(applied[1], std::string("put b=2"));

    minikv::raft::LogEntry e;
    CHECK(n2.GetLogEntryForTest(1, &e));
    CHECK_EQ(e.command, std::string("put a=1"));
    CHECK(n3.GetLogEntryForTest(2, &e));
    CHECK_EQ(e.command, std::string("put b=2"));
    minikv::test::Log("  basic replicate+commit OK");
  }

  minikv::test::LogStep("basic: non-leader Propose rejected");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode n1(MakeConfig(1, {2}), &net);
    minikv::raft::RaftNode n2(MakeConfig(2, {1}), &net);
    net.Register(&n1);
    net.Register(&n2);
    n1.SetElectionDeadlineForTest(100);
    n2.SetElectionDeadlineForTest(5000);
    n1.Tick(100);
    CHECK(n1.role() == minikv::raft::Role::kLeader);
    CHECK(!n2.Propose("x", nullptr).ok());
  }

  minikv::test::LogStep("minority leader cannot acknowledge a proposal");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode n1(MakeConfig(1, {2, 3}), &net);
    minikv::raft::RaftNode n2(MakeConfig(2, {1, 3}), &net);
    minikv::raft::RaftNode n3(MakeConfig(3, {1, 2}), &net);
    net.Register(&n1); net.Register(&n2); net.Register(&n3);
    ElectLeader1(&n1, &n2, &n3);
    net.Unregister(2); net.Unregister(3);
    CHECK(!n1.Propose("minority-write", nullptr).ok());
    CHECK_EQ(n1.commit_index(), static_cast<uint64_t>(0));
    CHECK(!n1.ConfirmLeadership().ok());
  }

  // ---------- 边界路径 ----------
  minikv::test::LogStep(
      "complex: divergent follower suffix overwritten by leader log");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode n1(MakeConfig(1, {2}), &net);
    minikv::raft::RaftNode n2(MakeConfig(2, {1}), &net);
    net.Register(&n1);
    net.Register(&n2);
    n1.SetElectionDeadlineForTest(100);
    n2.SetElectionDeadlineForTest(5000);
    n1.Tick(100);
    CHECK(n1.role() == minikv::raft::Role::kLeader);
    n1.Tick(140);
    n2.Tick(140);

    n2.AppendLocalForTest(/*term=*/9, "ghost");
    CHECK_EQ(n2.LastLogIndex(), static_cast<uint64_t>(1));

    minikv::raft::LogIndex idx = 0;
    CHECK_OK(n1.Propose("real-1", &idx));
    for (int t = 0; t < 8; ++t) {
      n1.Tick(200 + static_cast<uint64_t>(t) * 50);
      n2.Tick(200 + static_cast<uint64_t>(t) * 50);
    }

    minikv::raft::LogEntry e;
    CHECK(n2.GetLogEntryForTest(1, &e));
    CHECK_EQ(e.command, std::string("real-1"));
    CHECK_EQ(n2.commit_index(), static_cast<uint64_t>(1));
    minikv::test::Log("  conflict truncated; follower caught up");
  }

  minikv::test::LogStep(
      "complex: Figure-8 — old-term entry not committed until current-term entry "
      "reaches majority");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode solo(MakeConfig(1, {}), &net);
    net.Register(&solo);
    solo.SetElectionDeadlineForTest(100);
    solo.Tick(100);
    CHECK(solo.role() == minikv::raft::Role::kLeader);
    CHECK_EQ(solo.current_term(), static_cast<uint64_t>(1));

    solo.AppendLocalForTest(/*term=*/1, "old");
    // Leader 不会因 deadline 自动再选；先卸任再超时选主抬任期。
    solo.StepDownForTest(/*term=*/1);
    solo.SetElectionDeadlineForTest(100);
    solo.Tick(100);
    CHECK(solo.role() == minikv::raft::Role::kLeader);
    CHECK_EQ(solo.current_term(), static_cast<uint64_t>(2));
    CHECK_EQ(solo.commit_index(), static_cast<uint64_t>(0));

    solo.Tick(150);
    CHECK_EQ(solo.commit_index(), static_cast<uint64_t>(0));

    minikv::raft::LogIndex idx = 0;
    CHECK_OK(solo.Propose("new-term-2", &idx));
    CHECK_EQ(idx, static_cast<uint64_t>(2));
    CHECK_EQ(solo.commit_index(), static_cast<uint64_t>(2));
    minikv::test::Log("  Figure-8 held: old entry waited for current-term commit");
  }

  minikv::test::LogStep(
      "complex: vote prefers up-to-date log (stale candidate denied by richer node)");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode rich(MakeConfig(1, {2}), &net);
    minikv::raft::RaftNode poor(MakeConfig(2, {1}), &net);
    net.Register(&rich);
    net.Register(&poor);

    rich.AppendLocalForTest(3, "a");
    rich.AppendLocalForTest(3, "b");

    minikv::rpc::RaftRequestVoteReq req;
    req.term = 4;
    req.candidate_id = 2;
    req.last_log_index = 0;
    req.last_log_term = 0;
    auto resp = rich.OnRequestVote(req);
    CHECK_EQ(static_cast<int>(resp.vote_granted), 0);
    CHECK_EQ(resp.term, static_cast<uint64_t>(4));

    req.last_log_index = 2;
    req.last_log_term = 3;
    // 已在 term=4 投过票给别人？ voted_for 可能已是 0（BecomeFollower 清掉了）。
    // 上一次拒票时若 term 升到 4 且未投票，voted_for=0。
    resp = rich.OnRequestVote(req);
    CHECK_EQ(static_cast<int>(resp.vote_granted), 1);
    minikv::test::Log("  stale log denied; equal/newer log granted");
  }

  minikv::test::LogStep(
      "complex: partitioned follower catches up after re-register");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode n1(MakeConfig(1, {2, 3}), &net);
    minikv::raft::RaftNode n2(MakeConfig(2, {1, 3}), &net);
    minikv::raft::RaftNode n3(MakeConfig(3, {1, 2}), &net);
    net.Register(&n1);
    net.Register(&n2);
    net.Register(&n3);
    ElectLeader1(&n1, &n2, &n3);

    net.Unregister(3);
    CHECK_OK(n1.Propose("c1", nullptr));
    CHECK_OK(n1.Propose("c2", nullptr));
    CHECK_OK(n1.Propose("c3", nullptr));
    n1.Tick(200);
    n2.Tick(200);
    CHECK_EQ(n1.commit_index(), static_cast<uint64_t>(3));
    CHECK_EQ(n2.commit_index(), static_cast<uint64_t>(3));
    CHECK_EQ(n3.LastLogIndex(), static_cast<uint64_t>(0));

    net.Register(&n3);
    for (int t = 0; t < 10; ++t) {
      n1.Tick(300 + static_cast<uint64_t>(t) * 40);
      n2.Tick(300 + static_cast<uint64_t>(t) * 40);
      n3.Tick(300 + static_cast<uint64_t>(t) * 40);
    }
    CHECK_EQ(n3.commit_index(), static_cast<uint64_t>(3));
    minikv::raft::LogEntry e;
    CHECK(n3.GetLogEntryForTest(3, &e));
    CHECK_EQ(e.command, std::string("c3"));
    minikv::test::Log("  partitioned follower caught up to commit=3");
  }

  return minikv::test::Report("test_raft_replicate");
}
