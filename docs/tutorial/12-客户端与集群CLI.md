# 12 · 客户端库与集群 CLI

在 `ShardCluster` 之上提供应用侧 API 与命令行入口，并支持拓扑查询。

## 组件

| 组件 | 路径 | 作用 |
|------|------|------|
| `ClusterClient` | `src/cluster/cluster_client.*` | Put/Get/Delete、`ShardOfKey`、`Topology`；后台 Tick 可启用或关闭 |
| `RemoteClusterClient` | `src/cluster/kv_rpc.*` | 通过 TCP 调用 Put/Get/Delete |
| `KvRpcService` | `src/cluster/kv_rpc.*` | 解码 KV RPC 并调用分片集群 |
| 拓扑结构 | `src/cluster/topology.h` | 每分片 Leader 与副本角色 |
| CLI | `tools/minikv_cluster_cli.cpp` | put / get / delete / topology / which |
| 测试 | `tests/cluster/test_cluster_client.cpp` | 可控时钟与 auto-tick 路由 |

## 调用关系

```text
应用 / CLI
    │
    ▼
ClusterClient  ──Open──►  ShardCluster（每分片 Raft + 本地 DB）
    │                         │
    ├─ Put/Get/Delete         ├─ hash(key)%N 选分片
    ├─ Topology               └─ Leader Propose / Leader 读
    └─ 后台 Tick（按构造参数启用或关闭）
```

`ClusterClient` 是进程内教学入口。网络部署由 `minikv_server` 持有集群和 RPC 服务，`minikv_remote_cli` 通过 `RemoteClusterClient` 访问服务。

## CLI 示例

```bash
mkdir -p data/cluster
./build/minikv_cluster_cli ./data/cluster topology
./build/minikv_cluster_cli ./data/cluster which user:42
./build/minikv_cluster_cli ./data/cluster put user:42 alice
./build/minikv_cluster_cli ./data/cluster get user:42
./build/minikv_cluster_cli ./data/cluster --shards 3 delete user:42
```

网络入口：

```bash
./build/minikv_server ./data/server 9090 3 3
./build/minikv_remote_cli 127.0.0.1 9090 put user:42 alice
./build/minikv_remote_cli 127.0.0.1 9090 get user:42
```

`minikv_server` 在单个服务进程中承载多个分片和副本，适合观察路由。`minikv_node` 每个进程只承载一个 Raft 副本，用于跨进程共识：

```bash
./build/minikv_node 1 ./data/n1 9101 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
./build/minikv_node 2 ./data/n2 9102 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
./build/minikv_node 3 ./data/n3 9103 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
```

`minikv_node` 的启动参数给出初始成员。嵌入式部署可让 `TcpRaftTransport` 的地址表包含更多已知节点，再通过 `RaftNode::ChangeMembership` 以 joint consensus 改变投票成员。每个节点的数据目录包含独立的 Raft hard state、Raft 日志、快照和 LSM 文件。客户端写入只有在法定多数派持久化后才返回成功。

默认 `persist=true`：数据落在 `<cluster_root>/shard*/r*/`（Raft + 本地 KV），一次 `put` 的结果可由之后启动的进程通过 `get` 读取。`--no-persist` 表示不落盘，仅当前进程有效。

## 测试

```bash
./build/test_cluster_client
```

- **basic**：关闭 auto-tick，手动推进时钟，验证读写与每分片一个 Leader  
- **complex**：开启 auto-tick；多 key 覆盖多分片；`ShardOfKey` 与 `ShardOf` 一致  

## 与单机 CLI 的分工

| 工具 | 对象 |
|------|------|
| `minikv_cli` | 单个本地 DB 目录 |
| `minikv_cluster_cli` | 多分片进程内集群 |
