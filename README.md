# MiniKV

用 C++17 实现的键值存储系统：单机 LSM 引擎，以及基于 Raft 的分布式服务。

- 设计说明：[docs/系统设计说明书.md](docs/系统设计说明书.md)  
- 教程：[docs/tutorial/README.md](docs/tutorial/README.md)  
- 构建与完整验证：[docs/build-and-test.md](docs/build-and-test.md)  

## 能力概览

- 单机：WAL、MemTable（SkipList）、非阻塞 Flush、SSTable、Size-Tiered Compaction、CLI 与 Benchmark  
- 分布式：Raft（选举、复制、持久化、状态机 Apply、快照、joint consensus 成员变更）、静态分片、TCP RPC、远程客户端、故障演示  

测试目录：`tests/engine/`、`tests/rpc/`、`tests/raft/`、`tests/cluster/`。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 快速试用

```bash
mkdir -p data/demo data/cluster
./build/minikv_cli ./data/demo put user:1 alice
./build/minikv_cli ./data/demo get user:1

./build/minikv_cluster_cli ./data/cluster topology
./build/minikv_cluster_cli ./data/cluster put user:1 alice
./build/minikv_cluster_cli ./data/cluster get user:1
```

远程服务由服务进程和网络客户端组成：

```bash
./build/minikv_server ./data/server 9090 4 3
./build/minikv_remote_cli 127.0.0.1 9090 put user:1 alice
./build/minikv_remote_cli 127.0.0.1 9090 get user:1
```

三个独立进程组成一个初始为三成员的 Raft group：

```bash
./build/minikv_node 1 ./data/n1 9101 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
./build/minikv_node 2 ./data/n2 9102 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
./build/minikv_node 3 ./data/n3 9103 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
```

`minikv_remote_cli` 连接当前 Leader 的端口。Follower 返回 `not leader`，不会代替 Leader 确认请求。

## 整机串测与 Benchmark

```bash
./build/test_engine_e2e
./build/test_cluster_e2e

./scripts/demo_failover.sh
./build/minikv_bench --ops 5000 --value-bytes 100 --db ./data/bench
./build/minikv_cluster_bench --ops 2000 --shards 4 --db ./data/cluster_bench
```

说明见 [docs/benchmark.md](docs/benchmark.md) 与 [docs/tutorial/13-故障与分布式Bench.md](docs/tutorial/13-故障与分布式Bench.md)。

## 查看 WAL

```bash
./build/minikv_wal_dump ./data/demo/000001.log
```

## 嵌入示例

```cpp
#include "minikv/db.h"

minikv::Options options;
options.create_if_missing = true;

minikv::DB* db = nullptr;
minikv::Status s = minikv::DB::Open(options, "./data/demo", &db);
```

## 文档索引

| 文档 | 内容 |
|------|------|
| [系统设计说明书](docs/系统设计说明书.md) | 总体架构与能力范围 |
| [存储模型选型](docs/storage-models.md) | LSM 与其它模型对比 |
| [内存索引选型](docs/indexing.md) | SkipList 选型 |
| [Compaction 对比](docs/compaction.md) | Size-Tiered / Leveled |
| [Benchmark](docs/benchmark.md) | 整机串测与吞吐对比 |
| [构建与完整验证](docs/build-and-test.md) | Release、Sanitizer、测试与 Benchmark 命令 |
| [教程](docs/tutorial/README.md) | 按构建顺序的阅读路径 |
