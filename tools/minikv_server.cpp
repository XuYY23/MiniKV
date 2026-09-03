#include "cluster/kv_rpc.h"
#include "cluster/shard_cluster.h"
#include "rpc/server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> running{true};
void StopSignal(int) { running.store(false); }
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: " << argv[0]
              << " <data-dir> <port> [shards] [replicas]\n";
    return 1;
  }
  minikv::cluster::ShardClusterOptions options;
  options.root_dir = argv[1];
  options.persist = true;
  options.num_shards = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 2;
  options.replicas_per_shard =
      argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 3;

  minikv::cluster::ShardCluster cluster(options);
  minikv::Status s = cluster.Open();
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }
  cluster.ElectReplica0Leaders(100);
  minikv::cluster::KvRpcService service(&cluster);
  minikv::rpc::RpcServer server;
  server.SetHandler([&](const std::string& req, std::string* resp) {
    return service.Handle(req, resp);
  });
  s = server.Start("0.0.0.0", static_cast<uint16_t>(std::atoi(argv[2])));
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }
  std::signal(SIGINT, StopSignal);
  std::signal(SIGTERM, StopSignal);
  std::cout << "MiniKV 服务已监听端口 " << server.BoundPort() << "\n";
  uint64_t now = 200;
  while (running.load()) {
    cluster.TickAll(now);
    now += 50;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  server.Stop();
  cluster.Close();
  return 0;
}
