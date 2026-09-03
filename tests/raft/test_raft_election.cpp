#include "raft/memory_transport.h"
#include "raft/node.h"
#include "raft/rpc_bridge.h"
#include "raft/types.h"
#include "rpc/client.h"
#include "rpc/codec.h"
#include "rpc/server.h"
#include "tests/testharness.h"

#include <memory>
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
  c.heartbeat_interval_ms = 50;
  return c;
}

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_raft_election (小节 D2 · Raft 选举)");

  minikv::test::LogStep("unit: RequestVote rejects older term; grants once per term");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode node(MakeConfig(1, {2, 3}), &net);
    net.Register(&node);

    minikv::rpc::RaftRequestVoteReq req;
    req.term = 1;
    req.candidate_id = 2;
    req.last_log_index = 0;
    req.last_log_term = 0;
    auto r1 = node.OnRequestVote(req);
    CHECK_EQ(static_cast<int>(r1.vote_granted), 1);
    CHECK_EQ(r1.term, static_cast<uint64_t>(1));
    CHECK_EQ(node.voted_for(), static_cast<uint64_t>(2));

    minikv::rpc::RaftRequestVoteReq other;
    other.term = 1;
    other.candidate_id = 3;
    auto r2 = node.OnRequestVote(other);
    CHECK_EQ(static_cast<int>(r2.vote_granted), 0);

    minikv::rpc::RaftRequestVoteReq old_term;
    old_term.term = 0;
    old_term.candidate_id = 3;
    auto r0 = node.OnRequestVote(old_term);
    CHECK_EQ(static_cast<int>(r0.vote_granted), 0);
  }

  minikv::test::LogStep("3-node memory cluster: node1 times out first -> Leader");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode n1(MakeConfig(1, {2, 3}), &net);
    minikv::raft::RaftNode n2(MakeConfig(2, {1, 3}), &net);
    minikv::raft::RaftNode n3(MakeConfig(3, {1, 2}), &net);
    net.Register(&n1);
    net.Register(&n2);
    net.Register(&n3);

    // 可控时钟：只让 1 先到期。
    n1.SetElectionDeadlineForTest(100);
    n2.SetElectionDeadlineForTest(1000);
    n3.SetElectionDeadlineForTest(1000);

    n1.Tick(100);
    CHECK(n1.role() == minikv::raft::Role::kLeader);
    CHECK_EQ(n1.current_term(), static_cast<uint64_t>(1));
    minikv::test::Log(std::string("  n1 role=") + minikv::raft::RoleName(n1.role()) +
                      " term=" + std::to_string(n1.current_term()));

    // 同步推进，让 Leader 发心跳，Follower 认可 leader_id。
    n1.Tick(150);
    n2.Tick(150);
    n3.Tick(150);
    CHECK(n2.role() == minikv::raft::Role::kFollower);
    CHECK(n3.role() == minikv::raft::Role::kFollower);
    CHECK_EQ(n2.leader_id(), static_cast<uint64_t>(1));
    CHECK_EQ(n3.leader_id(), static_cast<uint64_t>(1));
    CHECK_EQ(n2.current_term(), static_cast<uint64_t>(1));
    minikv::test::Log("  after heartbeat: n2/n3 follow leader=1");
  }

  minikv::test::LogStep("TCP RPC: RequestVote payload via RpcServer -> RaftNode");
  {
    minikv::raft::MemoryTransport net;
    minikv::raft::RaftNode node(MakeConfig(10, {11}), &net);
    net.Register(&node);

    minikv::rpc::RpcServer server;
    server.SetHandler([&](const std::string& req, std::string* resp) {
      return minikv::raft::HandleRaftRpcPayload(&node, req, resp);
    });
    CHECK_OK(server.Start("127.0.0.1", 0));
    const uint16_t port = server.BoundPort();
    CHECK(port > 0);

    minikv::rpc::RaftRequestVoteReq vote;
    vote.term = 2;
    vote.candidate_id = 11;
    vote.last_log_index = 0;
    vote.last_log_term = 0;
    std::string body;
    CHECK_OK(minikv::rpc::EncodeRaftRequestVoteReq(vote, &body));
    minikv::rpc::RpcHeader header;
    header.type = minikv::rpc::MsgType::kRaftRequestVoteReq;
    header.request_id = 5;
    std::string payload;
    CHECK_OK(minikv::rpc::EncodePayload(header, body, &payload));

    minikv::rpc::RpcClient client;
    CHECK_OK(client.Connect("127.0.0.1", port));
    std::string resp_payload;
    CHECK_OK(client.Call(payload, &resp_payload));

    minikv::rpc::RpcHeader rh;
    std::string rbody;
    CHECK_OK(minikv::rpc::DecodePayload(resp_payload, &rh, &rbody));
    CHECK(rh.type == minikv::rpc::MsgType::kRaftRequestVoteResp);
    minikv::rpc::RaftRequestVoteResp resp;
    CHECK_OK(minikv::rpc::DecodeRaftRequestVoteResp(rbody, &resp));
    CHECK_EQ(static_cast<int>(resp.vote_granted), 1);
    CHECK_EQ(resp.term, static_cast<uint64_t>(2));
    CHECK_EQ(node.voted_for(), static_cast<uint64_t>(11));

    client.Close();
    server.Stop();
  }

  return minikv::test::Report("test_raft_election");
}
