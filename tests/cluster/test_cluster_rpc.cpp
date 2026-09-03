#include "cluster/kv_rpc.h"
#include "cluster/shard_cluster.h"
#include "rpc/server.h"
#include "tests/testharness.h"

int main() {
  minikv::test::LogSection("TEST SUITE: test_cluster_rpc");
  minikv::cluster::ShardClusterOptions options;
  options.root_dir = minikv::test::MakeTestDir("cluster_rpc");
  options.num_shards = 3;
  options.replicas_per_shard = 3;
  minikv::cluster::ShardCluster cluster(options);
  CHECK_OK(cluster.Open());
  cluster.ElectReplica0Leaders(100);

  minikv::cluster::KvRpcService service(&cluster);
  minikv::rpc::RpcServer server;
  server.SetHandler([&](const std::string& req, std::string* resp) {
    return service.Handle(req, resp);
  });
  CHECK_OK(server.Start("127.0.0.1", 0));
  minikv::cluster::RemoteClusterClient client("127.0.0.1",
                                               server.BoundPort());
  CHECK_OK(client.Put("remote-key", "remote-value"));
  std::string value;
  CHECK_OK(client.Get("remote-key", &value));
  CHECK_EQ(value, std::string("remote-value"));
  CHECK_OK(client.Delete("remote-key"));
  CHECK(client.Get("remote-key", &value).IsNotFound());
  server.Stop();
  cluster.Close();
  return minikv::test::Report("test_cluster_rpc");
}
