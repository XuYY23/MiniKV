# 13 · 故障演示与分布式 Benchmark

用可控故障注入演示杀 Leader、丢多数派、分区追赶与分片故障域；并用对比数字说明 Raft / 分片路径的相对开销。

## 整机串测

单机：`test_engine_e2e`。分布式：

```bash
./build/test_cluster_e2e
```

覆盖：跨分片 Put/Get → 写中杀 Leader → 分区追赶 → 覆盖/删除 → 拓扑 → persist Close/Open → 相对吞吐。

## 故障注入 API

`ShardCluster`（`src/cluster/shard_cluster.*`）：

| 方法 | 含义 |
|------|------|
| `IsolateReplica(shard, r)` | 从传输层摘除；若是 Leader 则 StepDown；不再参与选主查找 |
| `RestoreReplica(shard, r)` | 重新加入传输层，可被心跳追赶 |
| `ElectAmongLive(...)` | 在存活副本上统一 term 后重新选主 |
| `TickLive` | 只推进存活副本的逻辑时钟 |

运行：

```bash
./scripts/demo_failover.sh
# 或
./build/test_failover
```

## 用例说明

1. **杀 Leader**：隔离当前 Leader → 存活副本重选 → Put/Get 继续  
2. **丢多数派**：隔离多数副本 → Put 失败 → 恢复一名副本 → 再选主写成功  
3. **分区追赶**：隔离 Follower，Leader 继续写；Restore 后通过 AppendEntries 追上  
4. **分片故障域**：只杀某一分片 Leader，其它分片仍可写  

## 分布式 Benchmark

```bash
./build/minikv_cluster_bench --ops 2000 --value-bytes 64 --shards 4 --db ./data/cluster_bench
```

| 行 | 含义 |
|----|------|
| `local_engine_put+get` | 单机 LSM，无复制 |
| `raft_1shard_put+get` | 1 分片 × 3 副本 Raft |
| `raft_Nshard_put+get` | N 分片（每分片独立 Raft） |

本工具使用进程内 `MemoryTransport`，Raft 日志与 KV WAL 均同步持久化。单分片使用一个工作线程，多分片按分片并行执行；结果不包含跨机网络延迟。

## 与设计目标的对应

- 杀掉 Leader 后可重新选主并继续写入  
- 单分片经 Raft 的写/读；多分片下不同 Raft group 并行处理各自的 key  
