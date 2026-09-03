#include "env.h"
#include "raft/memory_transport.h"
#include "raft/node.h"
#include "raft/storage.h"
#include "tests/testharness.h"

#include <vector>

namespace {

minikv::raft::RaftConfig Config(minikv::raft::NodeId id) {
  minikv::raft::RaftConfig config;
  config.id = id;
  for (uint64_t peer = 1; peer <= 3; ++peer) {
    if (peer != id) config.peers.push_back(peer);
  }
  config.election_timeout_min_ms = 100;
  config.election_timeout_max_ms = 100;
  return config;
}

void Cleanup(const std::string& dir) {
  for (const char* file : {"hard_state", "hard_state.tmp", "raft.log",
                           "raft.log.tmp", "snapshot", "snapshot.tmp"}) {
    minikv::test::RemoveFileIfExists(dir + "/" + file);
  }
}

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_raft_membership");
  minikv::raft::MemoryTransport network;
  minikv::raft::RaftNode n1(Config(1), &network);
  minikv::raft::RaftNode n2(Config(2), &network);
  minikv::raft::RaftNode n3(Config(3), &network);
  network.Register(&n1);
  network.Register(&n2);
  network.Register(&n3);
  n1.SetElectionDeadlineForTest(100);
  n1.Tick(100);
  CHECK(n1.role() == minikv::raft::Role::kLeader);

  minikv::test::LogStep("joint consensus removes a voter");
  CHECK_OK(n1.ChangeMembership({1, 2}));
  n1.Tick(200);
  CHECK_EQ(n1.voters().size(), static_cast<size_t>(2));
  CHECK_EQ(n2.voters().size(), static_cast<size_t>(2));
  network.Unregister(2);
  CHECK(!n1.Propose("minority", nullptr).ok());
  network.Register(&n2);
  CHECK_OK(n1.Propose("quorum", nullptr));

  minikv::test::LogStep("joint consensus adds the voter back after catch-up");
  CHECK_OK(n1.ChangeMembership({1, 2, 3}));
  n1.Tick(300);
  CHECK_EQ(n1.voters().size(), static_cast<size_t>(3));
  CHECK_EQ(n2.voters().size(), static_cast<size_t>(3));
  CHECK_EQ(n3.voters().size(), static_cast<size_t>(3));

  minikv::test::LogStep("committed configuration is recovered from the log");
  const std::string root = minikv::test::MakeTestDir("raft_membership");
  const std::string dir = root + "/node1";
  Cleanup(dir);
  if (!minikv::Env::Default()->FileExists(dir)) {
    CHECK_OK(minikv::Env::Default()->CreateDir(dir));
  }
  {
    minikv::raft::MemoryTransport persisted_network;
    minikv::raft::RaftStorage storage(dir);
    minikv::raft::RaftNode leader(Config(1), &persisted_network, &storage);
    minikv::raft::RaftNode follower(Config(2), &persisted_network);
    persisted_network.Register(&leader);
    persisted_network.Register(&follower);
    CHECK_OK(leader.InitStorage());
    leader.SetElectionDeadlineForTest(100);
    leader.Tick(100);
    CHECK_OK(leader.ChangeMembership({1}));
    CHECK_EQ(leader.voters().size(), static_cast<size_t>(1));
  }
  {
    minikv::raft::MemoryTransport persisted_network;
    minikv::raft::RaftStorage storage(dir);
    minikv::raft::RaftNode recovered(Config(1), &persisted_network, &storage);
    CHECK_OK(recovered.InitStorage());
    CHECK_EQ(recovered.voters().size(), static_cast<size_t>(1));
    CHECK_EQ(recovered.voters()[0], static_cast<uint64_t>(1));
  }

  return minikv::test::Report("test_raft_membership");
}
