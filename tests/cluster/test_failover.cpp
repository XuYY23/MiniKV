#include "cluster/shard_cluster.h"
#include "raft/types.h"
#include "tests/testharness.h"

#include <string>

namespace {

std::string FindKeyOnShard(minikv::cluster::ShardCluster* c,
                           minikv::cluster::ShardId want) {
  for (int i = 0; i < 2000; ++i) {
    const std::string k = "fail:" + std::to_string(i);
    if (c->ShardOfKey(k) == want) {
      return k;
    }
  }
  return "";
}

}  // namespace

int main() {
  minikv::test::LogSection(
      "TEST SUITE: test_failover (小节 D7 · 杀 Leader / 分区 / 恢复)");

  minikv::test::LogStep(
      "basic: kill shard0 Leader → re-elect → continue Put/Get");
  {
    const std::string root = minikv::test::MakeTestDir("failover_kill_leader");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 1;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);
    CHECK(cluster.LeaderOf(0) != nullptr);
    const minikv::raft::NodeId old_leader = cluster.LeaderOf(0)->id();
    minikv::test::Log("  initial leader id=" + std::to_string(old_leader));

    const std::string k = FindKeyOnShard(&cluster, 0);
    CHECK(!k.empty());
    CHECK_OK(cluster.Put(k, "before-kill"));
    cluster.TickAll(150);

    std::string v;
    CHECK_OK(cluster.Get(k, &v));
    CHECK_EQ(v, std::string("before-kill"));

    // 杀掉 replica0（当前 Leader）。
    CHECK_OK(cluster.IsolateReplica(0, 0));
    CHECK(cluster.LeaderOf(0) == nullptr);
    minikv::test::Log("  isolated replica0; no leader");

    // 存活副本中让 replica1 优先当选。
    cluster.ElectAmongLive(0, /*prefer_replica=*/1, /*elect_at_ms=*/300);
    CHECK(cluster.LeaderOf(0) != nullptr);
    const minikv::raft::NodeId new_leader = cluster.LeaderOf(0)->id();
    CHECK(new_leader != old_leader);
    minikv::test::Log("  new leader id=" + std::to_string(new_leader));

    CHECK_OK(cluster.Put(k, "after-kill"));
    cluster.TickLive(400);
    CHECK_OK(cluster.Get(k, &v));
    CHECK_EQ(v, std::string("after-kill"));
    cluster.Close();
    minikv::test::Log("  write survived Leader failover");
  }

  minikv::test::LogStep(
      "basic: majority lost → Put fails; restore → elect & write again");
  {
    const std::string root = minikv::test::MakeTestDir("failover_majority");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 1;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);

    const std::string k = FindKeyOnShard(&cluster, 0);
    CHECK_OK(cluster.Put(k, "ok"));
    cluster.TickAll(150);

    // 隔离 2/3，只剩 1 个存活 → 无法形成多数派选主。
    CHECK_OK(cluster.IsolateReplica(0, 0));
    CHECK_OK(cluster.IsolateReplica(0, 1));
    cluster.ElectAmongLive(0, 2, 300);
    CHECK(cluster.LeaderOf(0) == nullptr);
    const minikv::Status bad = cluster.Put(k, "should-fail");
    CHECK(!bad.ok());
    minikv::test::Log("  Put without majority: " + bad.ToString());

    CHECK_OK(cluster.RestoreReplica(0, 1));
    cluster.ElectAmongLive(0, 1, 500);
    CHECK(cluster.LeaderOf(0) != nullptr);
    CHECK_OK(cluster.Put(k, "recovered"));
    cluster.TickLive(600);
    std::string v;
    CHECK_OK(cluster.Get(k, &v));
    CHECK_EQ(v, std::string("recovered"));
    cluster.Close();
    minikv::test::Log("  majority restored; writes resume");
  }

  minikv::test::LogStep(
      "isolated old Leader cannot acknowledge writes or serve reads");
  {
    const std::string root = minikv::test::MakeTestDir("failover_stale_leader");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 1;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);
    const std::string key = FindKeyOnShard(&cluster, 0);
    CHECK_OK(cluster.Put(key, "committed"));
    CHECK_OK(cluster.IsolateReplica(0, 1));
    CHECK_OK(cluster.IsolateReplica(0, 2));
    CHECK(!cluster.Put(key, "minority").ok());
    std::string value;
    CHECK(!cluster.Get(key, &value).ok());
    cluster.Close();
  }

  minikv::test::LogStep(
      "complex: partitioned follower catches up after Restore");
  {
    const std::string root = minikv::test::MakeTestDir("failover_partition");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 1;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);

    // 隔离 Follower（replica2），Leader 继续写入。
    CHECK_OK(cluster.IsolateReplica(0, 2));
    CHECK(cluster.LeaderOf(0) != nullptr);

    const std::string k = FindKeyOnShard(&cluster, 0);
    CHECK_OK(cluster.Put(k, "p1"));
    CHECK_OK(cluster.Put(k + ":b", "p2"));
    cluster.TickLive(200);

    // 分区期间落后副本本地 DB 不应已有最新值（Leader 读模型下 Get 仍走 Leader）。
    std::string v;
    CHECK_OK(cluster.Get(k, &v));
    CHECK_EQ(v, std::string("p1"));

    minikv::DB* lag = cluster.DbOf(0, 2);
    CHECK(lag != nullptr);
    const minikv::Status lag_s = lag->Get(minikv::ReadOptions(), k, &v);
    CHECK(lag_s.IsNotFound() || v != "p1");
    minikv::test::Log("  lagging replica DB not up-to-date during partition");

    CHECK_OK(cluster.RestoreReplica(0, 2));
    for (int t = 0; t < 12; ++t) {
      cluster.TickAll(300 + static_cast<uint64_t>(t) * 40);
    }
    CHECK_OK(lag->Get(minikv::ReadOptions(), k, &v));
    CHECK_EQ(v, std::string("p1"));
    CHECK_OK(lag->Get(minikv::ReadOptions(), k + ":b", &v));
    CHECK_EQ(v, std::string("p2"));
    cluster.Close();
    minikv::test::Log("  restored follower caught up via AppendEntries");
  }

  minikv::test::LogStep(
      "complex: multi-shard — kill one shard Leader does not block other shard");
  {
    const std::string root = minikv::test::MakeTestDir("failover_multishard");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 2;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);

    std::string k0 = FindKeyOnShard(&cluster, 0);
    std::string k1 = FindKeyOnShard(&cluster, 1);
    CHECK(!k0.empty());
    CHECK(!k1.empty());

    CHECK_OK(cluster.IsolateReplica(0, 0));
    // shard0 无 Leader；shard1 仍可写。
    CHECK(cluster.LeaderOf(0) == nullptr);
    CHECK(cluster.LeaderOf(1) != nullptr);
    CHECK_OK(cluster.Put(k1, "shard1-alive"));
    cluster.TickLive(200);
    const minikv::Status s0 = cluster.Put(k0, "blocked");
    CHECK(!s0.ok());
    minikv::test::Log("  shard0 Put blocked: " + s0.ToString());

    std::string v;
    CHECK_OK(cluster.Get(k1, &v));
    CHECK_EQ(v, std::string("shard1-alive"));

    cluster.ElectAmongLive(0, 1, 400);
    CHECK_OK(cluster.Put(k0, "shard0-back"));
    cluster.TickLive(500);
    CHECK_OK(cluster.Get(k0, &v));
    CHECK_EQ(v, std::string("shard0-back"));
    cluster.Close();
    minikv::test::Log("  shard failure is isolated; other shards continue");
  }

  return minikv::test::Report("test_failover");
}
