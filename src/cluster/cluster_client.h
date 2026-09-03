#pragma once

#include "cluster/shard_cluster.h"
#include "cluster/topology.h"
#include "minikv/status.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace minikv {
namespace cluster {

// 面向应用的集群客户端：路由 + Put/Get/Delete + 拓扑查询。
// 进程内嵌入 ShardCluster；可选后台 Tick 维持选举 / 心跳。
class ClusterClient {
 public:
  ClusterClient();
  ~ClusterClient();

  ClusterClient(const ClusterClient&) = delete;
  ClusterClient& operator=(const ClusterClient&) = delete;

  Status Open(const ShardClusterOptions& opt);
  void Close();

  Status Put(const std::string& key, const std::string& value);
  Status Get(const std::string& key, std::string* value);
  Status Delete(const std::string& key);

  ShardId ShardOfKey(const std::string& key) const;
  ClusterTopology Topology() const;

  // 测试可关掉后台时钟，改为手动 Tick。
  void SetAutoTick(bool on);
  void TickForTest(uint64_t now_ms);
  void ElectLeadersForTest(uint64_t elect_at_ms);

  ShardCluster* cluster_for_test() { return cluster_.get(); }

 private:
  void TickLoop();

  std::unique_ptr<ShardCluster> cluster_;
  std::atomic<bool> running_{false};
  std::atomic<bool> auto_tick_{true};
  std::atomic<uint64_t> logical_now_ms_{0};
  std::thread tick_thread_;
};

}  // namespace cluster
}  // namespace minikv
