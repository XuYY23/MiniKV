#include "bench/bench_common.h"
#include "cluster/shard_cluster.h"
#include "raft/types.h"
#include "tests/testharness.h"

#include <set>
#include <string>

// 分布式整机串测（对齐单机 test_engine_e2e）：
// 跨分片写读 → 写中杀主切换 → 分区追赶 → 覆盖/删除校验 → 拓扑 →
// 独立子目录 persist Close/Open 读回 → 相对吞吐。

namespace {

uint32_t LeaderReplicaIndex(minikv::cluster::ShardCluster* c,
                            minikv::cluster::ShardId shard,
                            uint32_t replicas) {
  for (uint32_t r = 0; r < replicas; ++r) {
    minikv::raft::RaftNode* n = c->NodeOf(shard, r);
    if (n != nullptr && n->role() == minikv::raft::Role::kLeader) {
      return r;
    }
  }
  return replicas;  // invalid
}

std::string KeyOnShard(minikv::cluster::ShardCluster* c,
                       minikv::cluster::ShardId want) {
  for (int i = 0; i < 8000; ++i) {
    const std::string k = "route:" + std::to_string(i);
    if (c->ShardOfKey(k) == want) {
      return k;
    }
  }
  return "";
}

}  // namespace

int main() {
  minikv::test::LogSection(
      "TEST SUITE: test_cluster_e2e (分布式整机串测 · 生产路径模拟)");

  const std::string root = minikv::test::MakeTestDir("cluster_e2e");
  constexpr int kN = 400;
  constexpr size_t kValSize = 48;
  constexpr uint32_t kShards = 3;
  constexpr uint32_t kReplicas = 3;

  // ----- 主路径：进程内集群（与单机 e2e 一样强调功能串测，不依赖脏盘）-----
  minikv::cluster::ShardClusterOptions opt;
  opt.root_dir = root + "/live";
  opt.num_shards = kShards;
  opt.replicas_per_shard = kReplicas;
  opt.persist = false;

  minikv::Env* env = minikv::Env::Default();
  if (!env->FileExists(opt.root_dir)) {
    CHECK_OK(env->CreateDir(opt.root_dir));
  }

  minikv::cluster::ShardCluster cluster(opt);
  CHECK_OK(cluster.Open());
  cluster.ElectReplica0Leaders(100);
  for (uint32_t s = 0; s < kShards; ++s) {
    if (cluster.LeaderOf(s) == nullptr) {
      minikv::test::Log("  FATAL: no leader on shard " + std::to_string(s));
      return minikv::test::Report("test_cluster_e2e");
    }
  }
  minikv::test::Log("  shards=" + std::to_string(kShards) +
                    " replicas=" + std::to_string(kReplicas) +
                    " leaders elected");

  minikv::test::LogStep("E2E sequential Put across shards (hash routing)");
  std::set<minikv::cluster::ShardId> used_shards;
  const uint64_t t0 = minikv::bench::NowMicros();
  for (int i = 0; i < kN; ++i) {
    const std::string k = minikv::bench::MakeKey(i);
    used_shards.insert(cluster.ShardOfKey(k));
    CHECK_OK_QUIET(cluster.Put(k, minikv::bench::MakeValue(i, kValSize)));
    if ((i & 63) == 0) {
      cluster.TickAll(200 + static_cast<uint64_t>(i));
    }
  }
  cluster.TickAll(2000);
  const uint64_t t1 = minikv::bench::NowMicros();
  minikv::bench::PrintStats(
      {"cluster.seq_write", static_cast<uint64_t>(kN), t1 - t0});
  CHECK_EQ(static_cast<int>(used_shards.size()), static_cast<int>(kShards));

  minikv::test::LogStep("E2E point Get all keys (Leader read)");
  std::string value;
  const uint64_t t2 = minikv::bench::NowMicros();
  for (int i = 0; i < kN; ++i) {
    CHECK_OK_QUIET(cluster.Get(minikv::bench::MakeKey(i), &value));
    CHECK_EQ_QUIET(value, minikv::bench::MakeValue(i, kValSize));
  }
  const uint64_t t3 = minikv::bench::NowMicros();
  minikv::bench::PrintStats(
      {"cluster.point_get", static_cast<uint64_t>(kN), t3 - t2});

  minikv::test::LogStep(
      "E2E kill current shard0 Leader mid-workload, re-elect, continue writes");
  {
    const minikv::raft::NodeId old = cluster.LeaderOf(0)->id();
    const uint32_t leader_rep =
        LeaderReplicaIndex(&cluster, 0, kReplicas);
    CHECK(leader_rep < kReplicas);
    CHECK_OK(cluster.IsolateReplica(0, leader_rep));
    CHECK(cluster.LeaderOf(0) == nullptr);

    const uint32_t prefer = (leader_rep + 1) % kReplicas;
    cluster.ElectAmongLive(0, prefer, 3000);
    if (cluster.LeaderOf(0) == nullptr) {
      minikv::test::Log("  FATAL: re-elect failed");
      return minikv::test::Report("test_cluster_e2e");
    }
    CHECK(cluster.LeaderOf(0)->id() != old);
    minikv::test::Log("  shard0 leader " + std::to_string(old) + " (rep" +
                      std::to_string(leader_rep) + ") -> " +
                      std::to_string(cluster.LeaderOf(0)->id()));

    constexpr int kExtra = 80;
    for (int i = kN; i < kN + kExtra; ++i) {
      CHECK_OK_QUIET(cluster.Put(minikv::bench::MakeKey(i),
                                 minikv::bench::MakeValue(i, kValSize)));
      if ((i & 15) == 0) {
        cluster.TickLive(4000 + static_cast<uint64_t>(i));
      }
    }
    cluster.TickLive(5000);
    for (int i = kN; i < kN + kExtra; ++i) {
      CHECK_OK_QUIET(cluster.Get(minikv::bench::MakeKey(i), &value));
      CHECK_EQ_QUIET(value, minikv::bench::MakeValue(i, kValSize));
    }
    CHECK_OK(cluster.Get(minikv::bench::MakeKey(0), &value));
    CHECK_EQ(value, minikv::bench::MakeValue(0, kValSize));

    CHECK_OK(cluster.RestoreReplica(0, leader_rep));
    for (int t = 0; t < 20; ++t) {
      cluster.TickAll(5500 + static_cast<uint64_t>(t) * 40);
    }
    minikv::test::Log("  post-failover writes+reads OK; old leader restored");
  }

  minikv::test::LogStep(
      "E2E partition follower on shard1, write, restore & catch-up");
  {
    CHECK_OK(cluster.IsolateReplica(1, 2));
    const std::string k_on_1 = KeyOnShard(&cluster, 1);
    CHECK(!k_on_1.empty());
    CHECK_OK(cluster.Put(k_on_1, "catch-me"));
    cluster.TickLive(7000);

    minikv::DB* lag = cluster.DbOf(1, 2);
    CHECK(lag != nullptr);
    const minikv::Status lag_s =
        lag->Get(minikv::ReadOptions(), k_on_1, &value);
    CHECK(lag_s.IsNotFound() || value != "catch-me");

    CHECK_OK(cluster.RestoreReplica(1, 2));
    for (int t = 0; t < 15; ++t) {
      cluster.TickAll(7500 + static_cast<uint64_t>(t) * 40);
    }
    CHECK_OK(lag->Get(minikv::ReadOptions(), k_on_1, &value));
    CHECK_EQ(value, std::string("catch-me"));
    minikv::test::Log("  catch-up OK key=" + k_on_1);
  }

  minikv::test::LogStep("E2E overwrite + delete subset, verify final view");
  const int kTotal = kN + 80;
  for (int i = 0; i < kTotal; i += 2) {
    CHECK_OK_QUIET(cluster.Put(
        minikv::bench::MakeKey(i),
        minikv::bench::MakeValue(i + 200000, kValSize)));
  }
  for (int i = 1; i < kTotal; i += 4) {
    CHECK_OK_QUIET(cluster.Delete(minikv::bench::MakeKey(i)));
  }
  cluster.TickLive(9000);

  int n_del = 0, n_ow = 0, n_orig = 0;
  for (int i = 0; i < kTotal; ++i) {
    minikv::Status s = cluster.Get(minikv::bench::MakeKey(i), &value);
    if (i % 4 == 1) {
      ++n_del;
      CHECK(s.IsNotFound());
    } else if (i % 2 == 0) {
      ++n_ow;
      CHECK_OK_QUIET(s);
      CHECK_EQ_QUIET(value,
                     minikv::bench::MakeValue(i + 200000, kValSize));
    } else {
      ++n_orig;
      CHECK_OK_QUIET(s);
      CHECK_EQ_QUIET(value, minikv::bench::MakeValue(i, kValSize));
    }
  }
  minikv::test::Log("  final view deleted=" + std::to_string(n_del) +
                    " overwritten=" + std::to_string(n_ow) +
                    " original=" + std::to_string(n_orig));

  minikv::test::LogStep("E2E topology: one Leader per shard");
  {
    const auto topo = cluster.Topology();
    CHECK_EQ(static_cast<int>(topo.size()), static_cast<int>(kShards));
    for (const auto& sh : topo) {
      CHECK(sh.leader_id != 0);
      int leaders = 0;
      for (const auto& r : sh.replicas) {
        if (r.role == minikv::raft::Role::kLeader) {
          ++leaders;
        }
      }
      CHECK_EQ(leaders, 1);
    }
  }
  cluster.Close();

  // ----- 持久化子路径：小规模 Close/Open（模拟进程重启读回）-----
  minikv::test::LogStep("E2E persist sub-cluster: Put → Close → Open → Get");
  {
    const std::string proot = root + "/persist";
    if (!env->FileExists(proot)) {
      CHECK_OK(env->CreateDir(proot));
    }
    minikv::cluster::ShardClusterOptions popt;
    popt.root_dir = proot;
    popt.num_shards = 2;
    popt.replicas_per_shard = 3;
    popt.persist = true;

    {
      minikv::cluster::ShardCluster c1(popt);
      CHECK_OK(c1.Open());
      c1.ElectReplica0Leaders(100);
      CHECK_OK(c1.Put("user:1", "alice"));
      CHECK_OK(c1.Put("user:2", "bob"));
      c1.TickAll(300);
      CHECK_OK(c1.Delete("user:2"));
      c1.TickAll(400);
      c1.Close();
    }
    {
      minikv::cluster::ShardCluster c2(popt);
      CHECK_OK(c2.Open());
      c2.ElectReplica0Leaders(100);
      c2.TickAll(200);
      CHECK_OK(c2.Get("user:1", &value));
      CHECK_EQ(value, std::string("alice"));
      CHECK(c2.Get("user:2", &value).IsNotFound());
      c2.Close();
    }
    minikv::test::Log("  persist reopen readback OK");
  }

  minikv::test::LogSection("Relative thrpt: local engine vs cluster.seq_write");
  {
    const std::string local = root + "/local_cmp";
    if (!env->FileExists(local)) {
      CHECK_OK(env->CreateDir(local));
    }
    minikv::bench::CleanupEngineDir(local);
    minikv::Options o;
    o.create_if_missing = true;
    o.sync = false;
    minikv::DB* db = nullptr;
    CHECK_OK(minikv::DB::Open(o, local, &db));
    minikv::WriteOptions w;
    const uint64_t a = minikv::bench::NowMicros();
    for (int i = 0; i < kN; ++i) {
      CHECK_OK_QUIET(db->Put(w, minikv::bench::MakeKey(i),
                             minikv::bench::MakeValue(i, kValSize)));
    }
    const uint64_t b = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"local_engine.seq_write", static_cast<uint64_t>(kN), b - a});
    db->Close().ok();
    delete db;
  }
  minikv::test::LogStep(
      "cluster path includes Raft majority replication; throughput is relative");

  return minikv::test::Report("test_cluster_e2e");
}
