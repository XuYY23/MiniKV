#include "cluster/cluster_client.h"
#include "cluster/shard_router.h"
#include "raft/types.h"
#include "tests/testharness.h"

#include <set>
#include <string>
#include <thread>
#include <chrono>

int main() {
  minikv::test::LogSection(
      "TEST SUITE: test_cluster_client (小节 D6 · 客户端库 / 拓扑)");

  minikv::test::LogStep("basic: Put/Get/Delete via ClusterClient");
  {
    const std::string root = minikv::test::MakeTestDir("cluster_client_basic");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 2;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ClusterClient client;
    client.SetAutoTick(false);  // 单测可控时钟
    CHECK_OK(client.Open(opt));
    // Open 已 ElectReplica0Leaders；再推进一轮复制。
    client.TickForTest(200);

    CHECK_OK(client.Put("alice", "1"));
    client.TickForTest(250);
    CHECK_OK(client.Put("bob", "2"));
    client.TickForTest(300);

    std::string v;
    CHECK_OK(client.Get("alice", &v));
    CHECK_EQ(v, std::string("1"));
    CHECK_OK(client.Get("bob", &v));
    CHECK_EQ(v, std::string("2"));

    CHECK_OK(client.Delete("alice"));
    client.TickForTest(350);
    CHECK(client.Get("alice", &v).IsNotFound());

    client.Close();
    minikv::test::Log("  ClusterClient Put/Get/Delete OK");
  }

  minikv::test::LogStep("basic: Topology reports one Leader per shard");
  {
    const std::string root = minikv::test::MakeTestDir("cluster_client_topo");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 2;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ClusterClient client;
    client.SetAutoTick(false);
    CHECK_OK(client.Open(opt));
    client.TickForTest(200);

    const auto topo = client.Topology();
    CHECK_EQ(static_cast<int>(topo.size()), 2);
    for (const auto& shard : topo) {
      CHECK(shard.leader_id != 0);
      int leaders = 0;
      for (const auto& r : shard.replicas) {
        if (r.role == minikv::raft::Role::kLeader) {
          ++leaders;
          CHECK_EQ(static_cast<int>(r.id), static_cast<int>(shard.leader_id));
        }
      }
      CHECK_EQ(leaders, 1);
      minikv::test::Log("  shard=" + std::to_string(shard.shard_id) +
                        " leader=" + std::to_string(shard.leader_id));
    }
    client.Close();
  }

  minikv::test::LogStep(
      "complex: auto-tick client routes keys across shards and survives");
  {
    const std::string root = minikv::test::MakeTestDir("cluster_client_auto");
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 3;
    opt.replicas_per_shard = 3;
    opt.root_dir = root;
    opt.persist = false;

    minikv::cluster::ClusterClient client;
    // 默认 auto_tick=true：模拟 CLI 后台心跳。
    CHECK_OK(client.Open(opt));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::set<minikv::cluster::ShardId> used;
    for (int i = 0; i < 30; ++i) {
      const std::string k = "item:" + std::to_string(i);
      const std::string val = "v" + std::to_string(i);
      used.insert(client.ShardOfKey(k));
      CHECK_OK(client.Put(k, val));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    for (int i = 0; i < 30; ++i) {
      const std::string k = "item:" + std::to_string(i);
      std::string val;
      CHECK_OK(client.Get(k, &val));
      CHECK_EQ(val, std::string("v" + std::to_string(i)));
    }
    CHECK(used.size() >= 2);
    minikv::test::Log("  keys covered shards=" + std::to_string(used.size()) +
                      " (expect >=2 of 3)");

    const auto topo = client.Topology();
    CHECK_EQ(static_cast<int>(topo.size()), 3);
    client.Close();
  }

  minikv::test::LogStep(
      "complex: ShardOfKey matches ShardRouter; which-key is deterministic");
  {
    minikv::cluster::ClusterClient client;
    client.SetAutoTick(false);
    minikv::cluster::ShardClusterOptions opt;
    opt.num_shards = 4;
    opt.replicas_per_shard = 3;
    opt.root_dir = minikv::test::MakeTestDir("cluster_client_which");
    opt.persist = false;
    CHECK_OK(client.Open(opt));

    for (int i = 0; i < 50; ++i) {
      const std::string k = "route:" + std::to_string(i);
      CHECK_EQ(static_cast<int>(client.ShardOfKey(k)),
               static_cast<int>(minikv::cluster::ShardOf(k, 4)));
    }
    client.Close();
    minikv::test::Log("  client.ShardOfKey == ShardOf(key, N)");
  }

  return minikv::test::Report("test_cluster_client");
}
