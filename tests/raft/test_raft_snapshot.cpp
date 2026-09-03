#include "env.h"
#include "raft/memory_transport.h"
#include "raft/node.h"
#include "raft/storage.h"
#include "tests/testharness.h"

#include <string>
#include <vector>

namespace {

class StringStateMachine : public minikv::raft::StateMachine {
 public:
  minikv::Status Apply(minikv::raft::LogIndex, minikv::raft::Term,
                       const std::string& command) override {
    if (!command.empty()) value += command;
    return minikv::Status::OK();
  }
  minikv::Status CreateSnapshot(std::string* data) override {
    *data = value;
    return minikv::Status::OK();
  }
  minikv::Status RestoreSnapshot(const std::string& data) override {
    value = data;
    return minikv::Status::OK();
  }
  std::string value;
};

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
  minikv::test::LogSection("TEST SUITE: test_raft_snapshot");

  minikv::test::LogStep("snapshot compacts the log and survives restart");
  const std::string root = minikv::test::MakeTestDir("raft_snapshot");
  const std::string dir = root + "/node";
  Cleanup(dir);
  if (!minikv::Env::Default()->FileExists(dir)) {
    CHECK_OK(minikv::Env::Default()->CreateDir(dir));
  }
  {
    minikv::raft::RaftConfig solo;
    solo.id = 1;
    solo.election_timeout_min_ms = 100;
    solo.election_timeout_max_ms = 100;
    minikv::raft::MemoryTransport network;
    minikv::raft::RaftStorage storage(dir);
    minikv::raft::RaftNode node(solo, &network, &storage);
    StringStateMachine state;
    CHECK_OK(node.InitStorage());
    CHECK_OK(node.SetStateMachine(&state));
    node.SetElectionDeadlineForTest(100);
    node.Tick(100);
    CHECK_OK(node.Propose("a", nullptr));
    CHECK_OK(node.Propose("b", nullptr));
    CHECK_OK(node.CreateSnapshot());
    CHECK_EQ(node.log_base_index(), static_cast<uint64_t>(2));
    CHECK_EQ(node.LastLogIndex(), static_cast<uint64_t>(2));
    minikv::raft::LogEntry compacted;
    CHECK(!node.GetLogEntryForTest(1, &compacted));
    CHECK_OK(node.Propose("c", nullptr));
  }
  {
    minikv::raft::RaftConfig solo;
    solo.id = 1;
    minikv::raft::MemoryTransport network;
    minikv::raft::RaftStorage storage(dir);
    minikv::raft::RaftNode node(solo, &network, &storage);
    StringStateMachine state;
    CHECK_OK(node.InitStorage());
    CHECK_EQ(node.log_base_index(), static_cast<uint64_t>(2));
    CHECK_EQ(node.LastLogIndex(), static_cast<uint64_t>(3));
    CHECK_OK(node.SetStateMachine(&state));
    CHECK_EQ(state.value, std::string("abc"));
  }

  minikv::test::LogStep("lagging follower catches up through InstallSnapshot");
  {
    minikv::raft::MemoryTransport network;
    minikv::raft::RaftNode n1(Config(1), &network);
    minikv::raft::RaftNode n2(Config(2), &network);
    minikv::raft::RaftNode n3(Config(3), &network);
    StringStateMachine s1, s2, s3;
    CHECK_OK(n1.SetStateMachine(&s1));
    CHECK_OK(n2.SetStateMachine(&s2));
    CHECK_OK(n3.SetStateMachine(&s3));
    network.Register(&n1);
    network.Register(&n2);
    network.Register(&n3);
    n1.SetElectionDeadlineForTest(100);
    n1.Tick(100);
    CHECK(n1.role() == minikv::raft::Role::kLeader);
    network.Unregister(3);
    CHECK_OK(n1.Propose("a", nullptr));
    CHECK_OK(n1.Propose("b", nullptr));
    CHECK_OK(n1.CreateSnapshot());
    CHECK_OK(n1.Propose("c", nullptr));
    network.Register(&n3);
    n1.Tick(200);
    CHECK_EQ(n3.log_base_index(), static_cast<uint64_t>(2));
    CHECK_EQ(n3.LastLogIndex(), static_cast<uint64_t>(3));
    CHECK_EQ(n3.commit_index(), static_cast<uint64_t>(3));
    CHECK_EQ(s3.value, std::string("abc"));
  }

  return minikv::test::Report("test_raft_snapshot");
}
