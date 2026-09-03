#include "raft/node.h"
#include "raft/rpc_bridge.h"
#include "raft/tcp_transport.h"
#include "rpc/server.h"
#include "tests/testharness.h"

#include <map>
#include <memory>

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

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_raft_tcp");
  minikv::raft::RaftNode* nodes[3] = {nullptr, nullptr, nullptr};
  minikv::rpc::RpcServer servers[3];
  for (int i = 0; i < 3; ++i) {
    servers[i].SetHandler([&, i](const std::string& req, std::string* resp) {
      return minikv::raft::HandleRaftRpcPayload(nodes[i], req, resp);
    });
    CHECK_OK(servers[i].Start("127.0.0.1", 0));
  }
  std::map<minikv::raft::NodeId, minikv::raft::Endpoint> endpoints;
  for (int i = 0; i < 3; ++i) {
    endpoints[static_cast<uint64_t>(i + 1)] =
        {"127.0.0.1", servers[i].BoundPort()};
  }
  std::unique_ptr<minikv::raft::TcpRaftTransport> transports[3];
  std::unique_ptr<minikv::raft::RaftNode> owned[3];
  StringStateMachine state_machines[3];
  for (int i = 0; i < 3; ++i) {
    transports[i] = std::make_unique<minikv::raft::TcpRaftTransport>(endpoints);
    minikv::raft::RaftConfig config;
    config.id = static_cast<uint64_t>(i + 1);
    for (int j = 0; j < 3; ++j) if (j != i) config.peers.push_back(j + 1);
    owned[i] = std::make_unique<minikv::raft::RaftNode>(config,
                                                        transports[i].get());
    nodes[i] = owned[i].get();
    CHECK_OK(nodes[i]->SetStateMachine(&state_machines[i]));
  }
  nodes[0]->SetElectionDeadlineForTest(100);
  nodes[1]->SetElectionDeadlineForTest(5000);
  nodes[2]->SetElectionDeadlineForTest(5000);
  nodes[0]->Tick(100);
  CHECK(nodes[0]->role() == minikv::raft::Role::kLeader);
  CHECK_OK(nodes[0]->Propose("tcp-command", nullptr));
  nodes[0]->Tick(200);
  CHECK_EQ(nodes[1]->commit_index(), static_cast<uint64_t>(1));
  CHECK_EQ(nodes[2]->commit_index(), static_cast<uint64_t>(1));

  minikv::test::LogStep("lagging TCP replica receives InstallSnapshot");
  nodes[2] = nullptr;
  CHECK_OK(nodes[0]->Propose("a", nullptr));
  CHECK_OK(nodes[0]->Propose("b", nullptr));
  CHECK_OK(nodes[0]->CreateSnapshot());
  CHECK_OK(nodes[0]->Propose("c", nullptr));
  nodes[2] = owned[2].get();
  nodes[0]->Tick(300);
  CHECK_EQ(nodes[2]->log_base_index(), static_cast<uint64_t>(3));
  CHECK_EQ(nodes[2]->LastLogIndex(), static_cast<uint64_t>(4));
  CHECK_EQ(state_machines[2].value,
           std::string("tcp-commandabc"));
  for (auto& server : servers) server.Stop();
  return minikv::test::Report("test_raft_tcp");
}
