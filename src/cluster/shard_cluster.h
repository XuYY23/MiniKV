#pragma once

#include "cluster/shard_router.h"
#include "cluster/topology.h"
#include "minikv/db.h"
#include "minikv/status.h"
#include "raft/memory_transport.h"
#include "raft/node.h"
#include "raft/state_machine.h"
#include "raft/storage.h"

#include <memory>
#include <string>
#include <vector>

namespace minikv {
namespace cluster {

struct ShardClusterOptions {
  uint32_t num_shards = 2;
  uint32_t replicas_per_shard = 3;
  std::string root_dir = "testdata/shard_cluster";  // 相对路径
  bool persist = false;  // false：纯内存 Raft（单测更快）
};

// 进程内多分片集群：每分片一组独立 Raft + 每副本一个本地 DB。
class ShardCluster {
 public:
  explicit ShardCluster(ShardClusterOptions opt);
  ~ShardCluster();

  Status Open();
  void Close();

  uint32_t num_shards() const { return opt_.num_shards; }
  ShardId ShardOfKey(const std::string& key) const;

  // 写必须打到该分片当前 Leader；读默认走 Leader 本地 DB（Leader 读模型）。
  Status Put(const std::string& key, const std::string& value);
  Status Delete(const std::string& key);
  Status Get(const std::string& key, std::string* value);

  // 推进所有 Raft 节点逻辑时钟（测试用）。
  void TickAll(uint64_t now_ms);

  // 让每个分片的 replica0 优先当选（可控测试时钟）。
  void ElectReplica0Leaders(uint64_t elect_at_ms);

  raft::RaftNode* LeaderOf(ShardId shard);
  raft::RaftNode* NodeOf(ShardId shard, uint32_t replica);
  DB* DbOf(ShardId shard, uint32_t replica);

  // 测试：该分片 Leader 日志条数。
  raft::LogIndex LeaderLogSize(ShardId shard);

  // 只读拓扑：每分片 Leader + 各副本角色。
  ClusterTopology Topology() const;

  // ---- 故障注入（进程内；Unregister 模拟宕机 / 分区）----
  // 隔离后该副本不再收发 Raft RPC，且不参与 Leader 查找。
  Status IsolateReplica(ShardId shard, uint32_t replica);
  Status RestoreReplica(ShardId shard, uint32_t replica);
  bool IsReplicaLive(ShardId shard, uint32_t replica) const;

  // 推进「仍存活」副本的时钟（已隔离副本也可 Tick，但不计入 FindLeader）。
  void TickLive(uint64_t now_ms);

  // 在存活副本中触发选举：优先让 prefer_replica 当选（若仍存活）。
  void ElectAmongLive(ShardId shard, uint32_t prefer_replica,
                      uint64_t elect_at_ms);

 private:
  struct Replica {
    raft::NodeId id = 0;
    std::unique_ptr<raft::RaftStorage> storage;
    std::unique_ptr<raft::RaftNode> node;
    std::unique_ptr<DB> db;
    std::unique_ptr<raft::KvStateMachine> sm;
    bool live = true;  // false = 已隔离（宕机/分区演示）
  };

  struct Group {
    ShardId shard_id = 0;
    raft::MemoryTransport transport;
    std::vector<Replica> replicas;
  };

  Status OpenShard(ShardId sid);
  raft::RaftNode* FindLeader(Group* g);

  ShardClusterOptions opt_;
  std::vector<std::unique_ptr<Group>> groups_;
  bool open_ = false;
};

}  // namespace cluster
}  // namespace minikv
