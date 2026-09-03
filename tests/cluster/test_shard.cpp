#include "cluster/shard_cluster.h"
#include "cluster/shard_router.h"
#include "tests/testharness.h"

#include <set>
#include <string>
#include <vector>

int main() {
  minikv::test::LogSection(
      "TEST SUITE: test_shard (小节 D5 · 静态分片 + 每分片 Raft)");

  minikv::test::LogStep("basic: ShardOf is stable and covers [0, N)");
  {
    constexpr uint32_t N = 4;
    std::set<minikv::cluster::ShardId> seen;
    int out_of_range = 0;
    for (int i = 0; i < 200; ++i) {
      const std::string key = "k" + std::to_string(i);
      const auto sid = minikv::cluster::ShardOf(key, N);
      if (sid >= N) {
        ++out_of_range;
      }
      seen.insert(sid);
    }
    CHECK_EQ(out_of_range, 0);
    CHECK_EQ(static_cast<int>(seen.size()), static_cast<int>(N));
    CHECK_EQ(minikv::cluster::ShardOf("same", N),
             minikv::cluster::ShardOf("same", N));
    minikv::test::Log("  hash%N covers all shards; stable for same key");
  }

  minikv::test::LogStep("basic: 2 shards Put/Get via routed Raft groups");
  {
    const std::string root = minikv::test::MakeTestDir("shard_basic");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 2;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);
    CHECK(cluster.LeaderOf(0) != nullptr);
    CHECK(cluster.LeaderOf(1) != nullptr);

    // 找两个落在不同分片的 key。
    std::string ka, kb;
    for (int i = 0; i < 1000; ++i) {
      const std::string k = "user:" + std::to_string(i);
      if (cluster.ShardOfKey(k) == 0) {
        ka = k;
      } else if (cluster.ShardOfKey(k) == 1) {
        kb = k;
      }
      if (!ka.empty() && !kb.empty()) {
        break;
      }
    }
    CHECK(!ka.empty());
    CHECK(!kb.empty());
    CHECK(cluster.ShardOfKey(ka) != cluster.ShardOfKey(kb));
    minikv::test::Log("  ka=" + ka + " shard=" +
                      std::to_string(cluster.ShardOfKey(ka)) + " kb=" + kb +
                      " shard=" + std::to_string(cluster.ShardOfKey(kb)));

    CHECK_OK(cluster.Put(ka, "A"));
    CHECK_OK(cluster.Put(kb, "B"));
    cluster.TickAll(200);

    std::string va, vb;
    CHECK_OK(cluster.Get(ka, &va));
    CHECK_OK(cluster.Get(kb, &vb));
    CHECK_EQ(va, std::string("A"));
    CHECK_EQ(vb, std::string("B"));

    CHECK(cluster.LeaderLogSize(0) >= 1);
    CHECK(cluster.LeaderLogSize(1) >= 1);
    cluster.Close();
  }

  minikv::test::LogStep(
      "complex: writes to different shards do not share one Raft log");
  {
    const std::string root = minikv::test::MakeTestDir("shard_isolate");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 2;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;
    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);

    std::string ka, kb;
    for (int i = 0; i < 2000; ++i) {
      const std::string k = "iso:" + std::to_string(i);
      if (cluster.ShardOfKey(k) == 0 && ka.empty()) {
        ka = k;
      }
      if (cluster.ShardOfKey(k) == 1 && kb.empty()) {
        kb = k;
      }
      if (!ka.empty() && !kb.empty()) {
        break;
      }
    }

    const auto log0_before = cluster.LeaderLogSize(0);
    const auto log1_before = cluster.LeaderLogSize(1);
    CHECK_OK(cluster.Put(ka, "only-shard0"));
    cluster.TickAll(150);
    const auto log0_mid = cluster.LeaderLogSize(0);
    const auto log1_mid = cluster.LeaderLogSize(1);
    CHECK(log0_mid > log0_before);
    CHECK_EQ(log1_mid, log1_before);

    CHECK_OK(cluster.Put(kb, "only-shard1"));
    cluster.TickAll(200);
    CHECK(cluster.LeaderLogSize(1) > log1_mid);
    // shard0 日志不应因 shard1 写入而增长（心跳可有空 Append，但 LastLogIndex
    // 不含空条目）。
    CHECK_EQ(cluster.LeaderLogSize(0), log0_mid);
    minikv::test::Log("  shard logs are independent");
    cluster.Close();
  }

  minikv::test::LogStep(
      "complex: Delete routed; Get after delete is NotFound on leader DB");
  {
    const std::string root = minikv::test::MakeTestDir("shard_del");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 2;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    minikv::cluster::ShardCluster cluster(opt);
    CHECK_OK(cluster.Open());
    cluster.ElectReplica0Leaders(100);

    const std::string key = "to-del";
    CHECK_OK(cluster.Put(key, "x"));
    cluster.TickAll(150);
    CHECK_OK(cluster.Delete(key));
    cluster.TickAll(200);

    std::string value;
    const minikv::Status s = cluster.Get(key, &value);
    CHECK(s.IsNotFound());
    cluster.Close();
  }

  return minikv::test::Report("test_shard");
}
