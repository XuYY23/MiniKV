#include "cluster/kv_rpc.h"
#include "minikv/db.h"
#include "raft/node.h"
#include "raft/state_machine.h"
#include "raft/storage.h"
#include "raft/tcp_transport.h"
#include "rpc/server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace {
std::atomic<bool> running{true};
void StopSignal(int) { running.store(false); }

bool ParseMember(const std::string& text, minikv::raft::NodeId* id,
                 minikv::raft::Endpoint* endpoint) {
  const size_t equal = text.find('=');
  const size_t colon = text.rfind(':');
  if (equal == std::string::npos || colon == std::string::npos ||
      equal >= colon) return false;
  *id = static_cast<uint64_t>(std::strtoull(text.substr(0, equal).c_str(),
                                            nullptr, 10));
  endpoint->host = text.substr(equal + 1, colon - equal - 1);
  endpoint->port = static_cast<uint16_t>(std::atoi(text.substr(colon + 1).c_str()));
  return *id != 0 && !endpoint->host.empty() && endpoint->port != 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "用法: " << argv[0]
              << " <node-id> <data-dir> <listen-port>"
                 " <id=host:port> [id=host:port ...]\n";
    return 1;
  }
  const auto id = static_cast<minikv::raft::NodeId>(std::strtoull(argv[1], nullptr, 10));
  const std::string data_dir = argv[2];
  const uint16_t listen_port = static_cast<uint16_t>(std::atoi(argv[3]));
  std::map<minikv::raft::NodeId, minikv::raft::Endpoint> endpoints;
  for (int i = 4; i < argc; ++i) {
    minikv::raft::NodeId member = 0;
    minikv::raft::Endpoint endpoint;
    if (!ParseMember(argv[i], &member, &endpoint)) {
      std::cerr << "无效成员: " << argv[i] << "\n";
      return 1;
    }
    endpoints[member] = std::move(endpoint);
  }
  if (endpoints.count(id) == 0 || endpoints[id].port != listen_port) {
    std::cerr << "成员表必须包含本节点及其监听端口\n";
    return 1;
  }
  std::filesystem::create_directories(data_dir + "/kv");
  std::filesystem::create_directories(data_dir + "/raft");

  minikv::Options db_options;
  db_options.sync = true;
  minikv::DB* raw_db = nullptr;
  minikv::Status s = minikv::DB::Open(db_options, data_dir + "/kv", &raw_db);
  if (!s.ok()) { std::cerr << s.ToString() << "\n"; return 1; }
  std::unique_ptr<minikv::DB> db(raw_db);
  minikv::raft::RaftStorage storage(data_dir + "/raft");
  minikv::raft::TcpRaftTransport transport(endpoints);
  minikv::raft::RaftConfig config;
  config.id = id;
  for (const auto& member : endpoints) if (member.first != id) config.peers.push_back(member.first);
  minikv::raft::RaftNode node(config, &transport, &storage);
  if (!(s = node.InitStorage()).ok()) { std::cerr << s.ToString() << "\n"; return 1; }
  minikv::raft::KvStateMachine state_machine(db.get());
  if (!(s = node.SetStateMachine(&state_machine)).ok()) { std::cerr << s.ToString() << "\n"; return 1; }
  minikv::cluster::RaftKvRpcService service(&node, db.get());
  minikv::rpc::RpcServer server;
  server.SetHandler([&](const std::string& req, std::string* resp) {
    return service.Handle(req, resp);
  });
  if (!(s = server.Start(endpoints[id].host, listen_port)).ok()) {
    std::cerr << s.ToString() << "\n"; return 1;
  }
  std::signal(SIGINT, StopSignal);
  std::signal(SIGTERM, StopSignal);
  std::cout << "MiniKV 节点 " << id << " 已监听 " << endpoints[id].host
            << ':' << listen_port << "\n";
  const auto start = std::chrono::steady_clock::now();
  while (running.load()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    node.Tick(static_cast<uint64_t>(elapsed));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  server.Stop();
  db->Close().ok();
  return 0;
}
