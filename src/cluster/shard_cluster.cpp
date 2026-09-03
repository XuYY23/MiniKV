#include "cluster/shard_cluster.h"

#include "env.h"
#include "minikv/options.h"

#include <algorithm>
#include <cstdio>

namespace minikv {
namespace cluster {
namespace {

Status EnsureDir(const std::string& path) {
  Env* env = Env::Default();
  if (env->FileExists(path)) {
    return Status::OK();
  }
  return env->CreateDir(path);
}

void CleanupDbFiles(const std::string& dbname) {
  Env* env = Env::Default();
  auto rm = [&](const std::string& p) {
    if (env->FileExists(p)) {
      env->DeleteFile(p).ok();
    }
  };
  rm(dbname + "/MANIFEST");
  rm(dbname + "/MANIFEST.tmp");
  for (int i = 1; i < 128; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%06d.log", dbname.c_str(), i);
    rm(buf);
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", dbname.c_str(), i);
    rm(buf);
  }
}

}  // namespace

ShardCluster::ShardCluster(ShardClusterOptions opt) : opt_(std::move(opt)) {}

ShardCluster::~ShardCluster() { Close(); }

ShardId ShardCluster::ShardOfKey(const std::string& key) const {
  return ShardOf(key, opt_.num_shards);
}

Status ShardCluster::Open() {
  if (open_) {
    return Status::InvalidArgument("already open");
  }
  if (opt_.num_shards == 0 || opt_.replicas_per_shard == 0) {
    return Status::InvalidArgument("bad shard/replica count");
  }
  Status s = EnsureDir(opt_.root_dir);
  if (!s.ok()) {
    return s;
  }
  groups_.clear();
  groups_.reserve(opt_.num_shards);
  for (uint32_t sid = 0; sid < opt_.num_shards; ++sid) {
    s = OpenShard(static_cast<ShardId>(sid));
    if (!s.ok()) {
      Close();
      return s;
    }
  }
  open_ = true;
  return Status::OK();
}

Status ShardCluster::OpenShard(ShardId sid) {
  auto g = std::make_unique<Group>();
  g->shard_id = sid;
  g->replicas.resize(opt_.replicas_per_shard);

  std::vector<raft::NodeId> all_ids;
  all_ids.reserve(opt_.replicas_per_shard);
  for (uint32_t r = 0; r < opt_.replicas_per_shard; ++r) {
    // 全局唯一：shard*100 + replica+1，便于日志辨认。
    all_ids.push_back(static_cast<raft::NodeId>(sid * 100 + r + 1));
  }

  for (uint32_t r = 0; r < opt_.replicas_per_shard; ++r) {
    Replica& rep = g->replicas[r];
    rep.id = all_ids[r];

    char shard_dir[64];
    std::snprintf(shard_dir, sizeof(shard_dir), "%s/shard%u",
                  opt_.root_dir.c_str(), sid);
    Status s = EnsureDir(shard_dir);
    if (!s.ok()) {
      return s;
    }
    char rep_dir[96];
    std::snprintf(rep_dir, sizeof(rep_dir), "%s/r%u", shard_dir, r);
    s = EnsureDir(rep_dir);
    if (!s.ok()) {
      return s;
    }
    const std::string db_path = std::string(rep_dir) + "/kv";
    s = EnsureDir(db_path);
    if (!s.ok()) {
      return s;
    }
    // 非持久化模式：每次 Open 清掉残留，保证单测可重复。
    // persist=true 时保留 kv/ 与 raft/，供 CLI 跨进程读回。
    if (!opt_.persist) {
      CleanupDbFiles(db_path);
    }

    Options options;
    options.create_if_missing = true;
    options.sync = false;
    DB* raw = nullptr;
    s = DB::Open(options, db_path, &raw);
    if (!s.ok()) {
      return s;
    }
    rep.db.reset(raw);
    rep.sm = std::make_unique<raft::KvStateMachine>(rep.db.get());

    raft::RaftConfig cfg;
    cfg.id = rep.id;
    for (uint32_t j = 0; j < opt_.replicas_per_shard; ++j) {
      if (j != r) {
        cfg.peers.push_back(all_ids[j]);
      }
    }
    cfg.election_timeout_min_ms = 150;
    cfg.election_timeout_max_ms = 150;
    cfg.heartbeat_interval_ms = 40;

    raft::RaftStorage* storage_ptr = nullptr;
    if (opt_.persist) {
      rep.storage =
          std::make_unique<raft::RaftStorage>(std::string(rep_dir) + "/raft");
      storage_ptr = rep.storage.get();
    }
    rep.node =
        std::make_unique<raft::RaftNode>(cfg, &g->transport, storage_ptr);
    if (storage_ptr != nullptr) {
      s = rep.node->InitStorage();
      if (!s.ok()) {
        return s;
      }
    }
    s = rep.node->SetStateMachine(rep.sm.get());
    if (!s.ok()) return s;
  }

  for (auto& rep : g->replicas) {
    g->transport.Register(rep.node.get());
  }
  groups_.push_back(std::move(g));
  return Status::OK();
}

void ShardCluster::Close() {
  for (auto& g : groups_) {
    if (!g) {
      continue;
    }
    for (auto& rep : g->replicas) {
      if (rep.db) {
        rep.db->Close().ok();
      }
    }
  }
  groups_.clear();
  open_ = false;
}

raft::RaftNode* ShardCluster::FindLeader(Group* g) {
  for (auto& rep : g->replicas) {
    if (!rep.live) {
      continue;
    }
    if (rep.node->role() == raft::Role::kLeader) {
      return rep.node.get();
    }
  }
  return nullptr;
}

raft::RaftNode* ShardCluster::LeaderOf(ShardId shard) {
  if (shard >= groups_.size()) {
    return nullptr;
  }
  return FindLeader(groups_[shard].get());
}

raft::RaftNode* ShardCluster::NodeOf(ShardId shard, uint32_t replica) {
  if (shard >= groups_.size() ||
      replica >= groups_[shard]->replicas.size()) {
    return nullptr;
  }
  return groups_[shard]->replicas[replica].node.get();
}

DB* ShardCluster::DbOf(ShardId shard, uint32_t replica) {
  if (shard >= groups_.size() ||
      replica >= groups_[shard]->replicas.size()) {
    return nullptr;
  }
  return groups_[shard]->replicas[replica].db.get();
}

raft::LogIndex ShardCluster::LeaderLogSize(ShardId shard) {
  raft::RaftNode* leader = LeaderOf(shard);
  if (leader == nullptr) {
    return 0;
  }
  return leader->LastLogIndex();
}

void ShardCluster::TickAll(uint64_t now_ms) {
  for (auto& g : groups_) {
    for (auto& rep : g->replicas) {
      rep.node->Tick(now_ms);
    }
  }
}

void ShardCluster::ElectReplica0Leaders(uint64_t elect_at_ms) {
  for (auto& g : groups_) {
    for (size_t i = 0; i < g->replicas.size(); ++i) {
      if (i == 0) {
        g->replicas[i].node->SetElectionDeadlineForTest(elect_at_ms);
      } else {
        g->replicas[i].node->SetElectionDeadlineForTest(elect_at_ms + 5000);
      }
    }
  }
  TickAll(elect_at_ms);
  TickAll(elect_at_ms + 50);
}

Status ShardCluster::Put(const std::string& key, const std::string& value) {
  if (!open_) {
    return Status::InvalidArgument("cluster not open");
  }
  const ShardId sid = ShardOfKey(key);
  raft::RaftNode* leader = LeaderOf(sid);
  if (leader == nullptr) {
    return Status::IOError("no leader for shard");
  }
  Status confirmed = leader->ConfirmLeadership();
  if (!confirmed.ok()) return confirmed;
  return leader->Propose(raft::KvCommand::EncodePut(key, value), nullptr);
}

Status ShardCluster::Delete(const std::string& key) {
  if (!open_) {
    return Status::InvalidArgument("cluster not open");
  }
  const ShardId sid = ShardOfKey(key);
  raft::RaftNode* leader = LeaderOf(sid);
  if (leader == nullptr) {
    return Status::IOError("no leader for shard");
  }
  Status confirmed = leader->ConfirmLeadership();
  if (!confirmed.ok()) return confirmed;
  return leader->Propose(raft::KvCommand::EncodeDelete(key), nullptr);
}

Status ShardCluster::Get(const std::string& key, std::string* value) {
  if (!open_ || value == nullptr) {
    return Status::InvalidArgument("bad get");
  }
  const ShardId sid = ShardOfKey(key);
  Group* g = groups_[sid].get();
  raft::RaftNode* leader = FindLeader(g);
  if (leader == nullptr) {
    return Status::IOError("no leader for shard");
  }
  Status confirmed = leader->ConfirmLeadership();
  if (!confirmed.ok()) return confirmed;
  // A quorum-confirmed Leader may serve the committed local state.
  for (auto& rep : g->replicas) {
    if (rep.node.get() == leader) {
      return rep.db->Get(ReadOptions(), key, value);
    }
  }
  return Status::IOError("leader db missing");
}

ClusterTopology ShardCluster::Topology() const {
  ClusterTopology out;
  if (!open_) {
    return out;
  }
  out.reserve(groups_.size());
  for (const auto& g : groups_) {
    ShardInfo info;
    info.shard_id = g->shard_id;
    for (const auto& rep : g->replicas) {
      ReplicaInfo ri;
      ri.id = rep.id;
      // 隔离副本对外标成 Follower，避免误报「死 Leader」。
      ri.role = rep.live ? rep.node->role() : raft::Role::kFollower;
      info.replicas.push_back(ri);
      if (rep.live && ri.role == raft::Role::kLeader) {
        info.leader_id = ri.id;
      }
    }
    out.push_back(info);
  }
  return out;
}

Status ShardCluster::IsolateReplica(ShardId shard, uint32_t replica) {
  if (!open_ || shard >= groups_.size() ||
      replica >= groups_[shard]->replicas.size()) {
    return Status::InvalidArgument("bad shard/replica");
  }
  Replica& rep = groups_[shard]->replicas[replica];
  if (!rep.live) {
    return Status::OK();
  }
  groups_[shard]->transport.Unregister(rep.id);
  // 若是 Leader，主动卸任，避免 FindLeader 仍命中「幽灵 Leader」。
  if (rep.node->role() == raft::Role::kLeader) {
    rep.node->StepDownForTest(rep.node->current_term());
  }
  rep.live = false;
  return Status::OK();
}

Status ShardCluster::RestoreReplica(ShardId shard, uint32_t replica) {
  if (!open_ || shard >= groups_.size() ||
      replica >= groups_[shard]->replicas.size()) {
    return Status::InvalidArgument("bad shard/replica");
  }
  Replica& rep = groups_[shard]->replicas[replica];
  if (rep.live) {
    return Status::OK();
  }
  groups_[shard]->transport.Register(rep.node.get());
  rep.live = true;
  return Status::OK();
}

bool ShardCluster::IsReplicaLive(ShardId shard, uint32_t replica) const {
  if (!open_ || shard >= groups_.size() ||
      replica >= groups_[shard]->replicas.size()) {
    return false;
  }
  return groups_[shard]->replicas[replica].live;
}

void ShardCluster::TickLive(uint64_t now_ms) {
  for (auto& g : groups_) {
    for (auto& rep : g->replicas) {
      if (rep.live) {
        rep.node->Tick(now_ms);
      }
    }
  }
}

void ShardCluster::ElectAmongLive(ShardId shard, uint32_t prefer_replica,
                                  uint64_t elect_at_ms) {
  if (!open_ || shard >= groups_.size()) {
    return;
  }
  Group* g = groups_[shard].get();
  // 对齐存活副本的最大 term，再卸任，避免孤岛空选举抬高任期后挡住恢复节点。
  raft::Term max_term = 0;
  for (auto& rep : g->replicas) {
    if (rep.live) {
      max_term = std::max(max_term, rep.node->current_term());
    }
  }
  for (auto& rep : g->replicas) {
    if (!rep.live) {
      continue;
    }
    if (rep.node->current_term() < max_term ||
        rep.node->role() != raft::Role::kFollower) {
      rep.node->StepDownForTest(max_term);
    }
  }
  for (size_t i = 0; i < g->replicas.size(); ++i) {
    if (!g->replicas[i].live) {
      continue;
    }
    if (i == prefer_replica) {
      g->replicas[i].node->SetElectionDeadlineForTest(elect_at_ms);
    } else {
      g->replicas[i].node->SetElectionDeadlineForTest(elect_at_ms + 5000);
    }
  }
  TickLive(elect_at_ms);
  TickLive(elect_at_ms + 50);
}

}  // namespace cluster
}  // namespace minikv
