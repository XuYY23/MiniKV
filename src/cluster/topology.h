#pragma once

#include "cluster/shard_router.h"
#include "minikv/status.h"
#include "raft/types.h"

#include <string>
#include <vector>

namespace minikv {
namespace cluster {

struct ReplicaInfo {
  raft::NodeId id = 0;
  raft::Role role = raft::Role::kFollower;
};

struct ShardInfo {
  ShardId shard_id = 0;
  raft::NodeId leader_id = 0;  // 0 = 未知
  std::vector<ReplicaInfo> replicas;
};

// 集群只读拓扑（排障 / CLI topology）。
using ClusterTopology = std::vector<ShardInfo>;

}  // namespace cluster
}  // namespace minikv
