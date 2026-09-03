#include "bench/bench_common.h"
#include "cluster/shard_cluster.h"
#include "minikv/db.h"
#include "minikv/status.h"

#include <cstdlib>
#include <iostream>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

void Usage(const char* argv0) {
  std::cerr
      << "用法: " << argv0
      << " [--ops N] [--value-bytes B] [--shards S] [--replicas R] [--db DIR]\n"
      << "  对比：单机 LSM vs 1 分片 Raft vs 多分片 Raft。\n"
      << "  默认: --ops 2000 --value-bytes 64 --shards 4 --replicas 3 "
         "--db ./data/cluster_bench\n";
}

int ParseInt(const char* s, int def) {
  if (s == nullptr || *s == '\0') {
    return def;
  }
  return std::atoi(s);
}

bool EnsureDir(minikv::Env* env, const std::string& path) {
  if (env->FileExists(path)) {
    return true;
  }
  minikv::Status s = env->CreateDir(path);
  return s.ok() || env->FileExists(path);
}

minikv::bench::RunStats BenchLocalEngine(const std::string& dir, int ops,
                                         int value_bytes) {
  minikv::bench::CleanupEngineDir(dir);
  minikv::Options options;
  options.create_if_missing = true;
  options.sync = false;
  minikv::DB* db = nullptr;
  minikv::Status s = minikv::DB::Open(options, dir, &db);
  if (!s.ok()) {
    std::cerr << "local open: " << s.ToString() << "\n";
    std::exit(1);
  }
  minikv::WriteOptions wopt;
  wopt.sync = true;
  minikv::ReadOptions ropt;
  const uint64_t t0 = minikv::bench::NowMicros();
  for (int i = 0; i < ops; ++i) {
    const std::string k = minikv::bench::MakeKey(i);
    const std::string v = minikv::bench::MakeValue(i, value_bytes);
    s = db->Put(wopt, k, v);
    if (!s.ok()) {
      std::cerr << "local put: " << s.ToString() << "\n";
      std::exit(1);
    }
  }
  for (int i = 0; i < ops; ++i) {
    std::string v;
    s = db->Get(ropt, minikv::bench::MakeKey(i), &v);
    if (!s.ok()) {
      std::cerr << "local get: " << s.ToString() << "\n";
      std::exit(1);
    }
  }
  const uint64_t t1 = minikv::bench::NowMicros();
  db->Close().ok();
  delete db;
  minikv::bench::RunStats st;
  st.name = "local_engine_put+get";
  st.ops = static_cast<uint64_t>(ops) * 2;
  st.micros = t1 - t0;
  return st;
}

minikv::bench::RunStats BenchCluster(const std::string& dir, uint32_t shards,
                                     uint32_t replicas, int ops,
                                     int value_bytes, const char* name) {
  if (!EnsureDir(minikv::Env::Default(), dir)) {
    std::cerr << "cannot create " << dir << "\n";
    std::exit(1);
  }
  minikv::cluster::ShardClusterOptions opt;
  opt.root_dir = dir + "/run-" + std::to_string(minikv::bench::NowMicros());
  opt.num_shards = shards;
  opt.replicas_per_shard = replicas;
  opt.persist = true;

  minikv::cluster::ShardCluster cluster(opt);
  minikv::Status s = cluster.Open();
  if (!s.ok()) {
    std::cerr << "cluster open: " << s.ToString() << "\n";
    std::exit(1);
  }
  cluster.ElectReplica0Leaders(100);

  std::vector<std::vector<std::string>> keys(shards);
  for (int i = 0; i < ops; ++i) {
    std::string key = "bench-" + std::to_string(i);
    keys[cluster.ShardOfKey(key)].push_back(std::move(key));
  }
  std::atomic<int> failures{0};
  const uint64_t t0 = minikv::bench::NowMicros();
  std::vector<std::thread> workers;
  for (uint32_t shard = 0; shard < shards; ++shard) {
    workers.emplace_back([&, shard] {
      for (size_t i = 0; i < keys[shard].size(); ++i) {
        const std::string value = minikv::bench::MakeValue(
            static_cast<int>(i), static_cast<size_t>(value_bytes));
        if (!cluster.Put(keys[shard][i], value).ok()) ++failures;
      }
      for (const auto& key : keys[shard]) {
        std::string value;
        if (!cluster.Get(key, &value).ok()) ++failures;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  if (failures.load() != 0) {
    std::cerr << "cluster operations failed: " << failures.load() << "\n";
    std::exit(1);
  }
  const uint64_t t1 = minikv::bench::NowMicros();
  cluster.Close();

  minikv::bench::RunStats st;
  st.name = name;
  st.ops = static_cast<uint64_t>(ops) * 2;
  st.micros = t1 - t0;
  return st;
}

}  // namespace

int main(int argc, char** argv) {
  int ops = 2000;
  int value_bytes = 64;
  int shards = 4;
  int replicas = 3;
  std::string dbroot = "./data/cluster_bench";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ops" && i + 1 < argc) {
      ops = ParseInt(argv[++i], ops);
    } else if (arg == "--value-bytes" && i + 1 < argc) {
      value_bytes = ParseInt(argv[++i], value_bytes);
    } else if (arg == "--shards" && i + 1 < argc) {
      shards = ParseInt(argv[++i], shards);
    } else if (arg == "--replicas" && i + 1 < argc) {
      replicas = ParseInt(argv[++i], replicas);
    } else if (arg == "--db" && i + 1 < argc) {
      dbroot = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      Usage(argv[0]);
      return 0;
    } else {
      Usage(argv[0]);
      return 1;
    }
  }

  minikv::Env* env = minikv::Env::Default();
  if (!EnsureDir(env, "./data") || !EnsureDir(env, dbroot)) {
    std::cerr << "cannot create " << dbroot << "\n";
    return 1;
  }

  std::printf(
      "MiniKV cluster bench  ops=%d value_bytes=%d shards=%d replicas=%d "
      "db=%s\n",
      ops, value_bytes, shards, replicas, dbroot.c_str());
  std::printf(
      "(put+get 各 ops 次；各路径均使用同步持久化，多分片并行执行)\n");

  {
    const auto st =
        BenchLocalEngine(dbroot + "/local", ops, value_bytes);
    minikv::bench::PrintStats(st);
  }
  {
    const auto st =
        BenchCluster(dbroot + "/raft1", 1, static_cast<uint32_t>(replicas), ops,
                     value_bytes, "raft_1shard_put+get");
    minikv::bench::PrintStats(st);
  }
  {
    char name[64];
    std::snprintf(name, sizeof(name), "raft_%dshard_put+get", shards);
    const auto st =
        BenchCluster(dbroot + "/raftN", static_cast<uint32_t>(shards),
                     static_cast<uint32_t>(replicas), ops, value_bytes, name);
    minikv::bench::PrintStats(st);
  }

  std::printf(
      "范围: Raft 传输为进程内 MemoryTransport；结果衡量持久化、复制与分片并行开销，"
      "不代表跨机网络性能。\n");
  return 0;
}
