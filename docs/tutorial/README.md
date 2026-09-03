# MiniKV 教程导读

本目录按项目构建顺序组织，[系统设计说明书](../系统设计说明书.md)描述完整架构与能力边界。

## 阅读顺序

1. [01-工程骨架与公共类型](01-工程骨架与公共类型.md)  
2. [../storage-models.md](../storage-models.md)  
3. [../indexing.md](../indexing.md)  
4. [02-MemTable与WAL](02-MemTable与WAL.md)  
5. [03-Flush与SSTable](03-Flush与SSTable.md)  
6. [04-Compaction](04-Compaction.md) / [../compaction.md](../compaction.md)  
7. [05-Benchmark与整机串测](05-Benchmark与整机串测.md) / [../benchmark.md](../benchmark.md)  
8. [06-并发读写](06-并发读写.md)  
9. [07-RPC 协议与服务](07-RPC骨架.md)  
10. [08-Raft选举](08-Raft选举.md)  
11. [09-Raft日志复制](09-Raft日志复制.md)  
12. [10-Raft持久化与Apply](10-Raft持久化与Apply.md)  
13. [11-静态分片](11-静态分片.md)  
14. [12-客户端与集群CLI](12-客户端与集群CLI.md)  
15. [13-故障与分布式Bench](13-故障与分布式Bench.md)  
16. 源码：`src/engine|rpc|raft|cluster/`；测试：`tests/`

## 构建与验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

mkdir -p data/demo data/cluster
./build/minikv_cli ./data/demo put user:1 alice
./build/minikv_cluster_cli ./data/cluster put user:1 alice
./build/minikv_server ./data/server 9090
./build/minikv_remote_cli 127.0.0.1 9090 get user:1
./scripts/demo_failover.sh
./build/minikv_cluster_bench --ops 1000 --db ./data/cluster_bench
```
