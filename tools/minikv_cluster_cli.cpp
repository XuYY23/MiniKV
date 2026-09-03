#include "cluster/cluster_client.h"
#include "raft/types.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

namespace {

void PrintUsage(const char* argv0) {
  std::cerr
      << "用法（相对路径；进程内嵌入多分片 Raft）:\n"
      << "  " << argv0
      << " <cluster_root> [--shards N] [--replicas R] put <key> <value>\n"
      << "  " << argv0
      << " <cluster_root> [--shards N] [--replicas R] get <key>\n"
      << "  " << argv0
      << " <cluster_root> [--shards N] [--replicas R] delete <key>\n"
      << "  " << argv0
      << " <cluster_root> [--shards N] [--replicas R] topology\n"
      << "  " << argv0
      << " <cluster_root> [--shards N] [--replicas R] which <key>\n"
      << "选项: --shards N  --replicas R  --no-persist\n";
}

void PrintTopology(const minikv::cluster::ClusterTopology& topo) {
  for (const auto& shard : topo) {
    std::cout << "shard " << shard.shard_id << " leader=" << shard.leader_id
              << "\n";
    for (const auto& r : shard.replicas) {
      std::cout << "  replica " << r.id << " "
                << minikv::raft::RoleName(r.role) << "\n";
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    PrintUsage(argv[0]);
    return 1;
  }

  minikv::cluster::ShardClusterOptions opt;
  opt.root_dir = argv[1];
  opt.persist = true;  // 默认落盘，便于跨进程 put 后再 get
  opt.num_shards = 2;
  opt.replicas_per_shard = 3;

  int i = 2;
  while (i < argc) {
    const std::string a = argv[i];
    if (a == "--shards" && i + 1 < argc) {
      opt.num_shards = static_cast<uint32_t>(std::atoi(argv[++i]));
      ++i;
    } else if (a == "--replicas" && i + 1 < argc) {
      opt.replicas_per_shard = static_cast<uint32_t>(std::atoi(argv[++i]));
      ++i;
    } else if (a == "--no-persist") {
      opt.persist = false;
      ++i;
    } else {
      break;
    }
  }

  if (i >= argc) {
    PrintUsage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[i++];

  minikv::cluster::ClusterClient client;
  minikv::Status s = client.Open(opt);
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }
  // 给心跳一轮时间，确保 Leader 稳定（Propose 依赖 Leader）。
  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  int code = 0;
  if (cmd == "put") {
    if (i + 1 >= argc) {
      PrintUsage(argv[0]);
      code = 1;
    } else {
      s = client.Put(argv[i], argv[i + 1]);
      if (!s.ok()) {
        std::cerr << s.ToString() << "\n";
        code = 1;
      } else {
        std::cout << "OK shard=" << client.ShardOfKey(argv[i]) << "\n";
      }
    }
  } else if (cmd == "get") {
    if (i >= argc) {
      PrintUsage(argv[0]);
      code = 1;
    } else {
      std::string value;
      s = client.Get(argv[i], &value);
      if (s.IsNotFound()) {
        std::cout << "(null)\n";
      } else if (!s.ok()) {
        std::cerr << s.ToString() << "\n";
        code = 1;
      } else {
        std::cout << value << "\n";
      }
    }
  } else if (cmd == "delete") {
    if (i >= argc) {
      PrintUsage(argv[0]);
      code = 1;
    } else {
      s = client.Delete(argv[i]);
      if (!s.ok()) {
        std::cerr << s.ToString() << "\n";
        code = 1;
      } else {
        std::cout << "OK\n";
      }
    }
  } else if (cmd == "topology") {
    PrintTopology(client.Topology());
  } else if (cmd == "which") {
    if (i >= argc) {
      PrintUsage(argv[0]);
      code = 1;
    } else {
      std::cout << "key=" << argv[i]
                << " shard=" << client.ShardOfKey(argv[i]) << "\n";
    }
  } else {
    PrintUsage(argv[0]);
    code = 1;
  }

  client.Close();
  return code;
}
