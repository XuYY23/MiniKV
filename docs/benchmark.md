# Benchmark 与整机串测

## 目标

1. **整机串测**：把 WAL → MemTable → Flush → SSTable → Compaction → 重启恢复串成一条可重复路径。  
2. **吞吐对比**：同一机器上对比  
   - `memory_map`：纯内存（无持久化）  
   - `simple_wal`：极简追加日志 + 内存 map  
   - `MiniKV`：完整单机引擎  

结果只在相同机器、构建类型、同步策略和负载参数下比较。单机工具使用非同步写来分离数据结构开销；分布式工具使用同步持久化来比较单副本与三副本提交成本。

## 运行方式

### 作为测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/test_engine_e2e
# 或
ctest --test-dir build -R engine_e2e --output-on-failure
```

日志中可见：

- 顺序写 / 点查吞吐  
- `sst_count` / `compaction_count`（确认 Flush / Compaction 已执行）  
- 覆盖写、删除、关闭再打开  
- 与两条基线的对比行  

### 作为独立压测工具

```bash
./build/minikv_bench --ops 5000 --value-bytes 100 --db ./data/bench
```

路径为相对路径；首次会创建 `./data/bench/`。

## 负载定义

### 单机工具

`minikv_bench` 依次测量内存 `std::map`、只追加 WAL 和完整 LSM。三者的耐久性不同，名称中明确标出其性质；该组数字用于分解组件成本，不构成同等持久化语义下的产品对比。

### 分布式工具

`minikv_cluster_bench` 的本地引擎和 Raft 状态机均使用同步 WAL。Raft 日志持久化开启。单分片使用一个工作线程，多分片为每个分片建立工作线程，总操作数保持不变。Raft 传输使用 `MemoryTransport`，因此结果包含共识与持久化成本，但不包含跨机网络延迟。

```bash
./build/minikv_cluster_bench --ops 600 --value-bytes 64 \
  --shards 4 --replicas 3 --db ./data/cluster_bench
```

## 结果字段

| 字段 | 含义 |
|------|------|
| `ops` | 完成的操作数；集群测试包含 Put 与 Get |
| `time_us` | 整段负载的单调时钟耗时 |
| `thrpt` | `ops / elapsed seconds` |
| `sst_count` | 测试结束时的 SST 数量 |
| `compaction_count` | 本轮运行触发的 Compaction 次数 |

正式记录包含以下环境字段：

```text
机器 / 系统：
编译：Release / Debug
命令：
结果：
```

## 相关文件

- `tests/engine/test_engine_e2e.cpp` — 串测与轻量 bench  
- `bench/minikv_bench.cpp` — 独立工具  
- `bench/bench_common.h` — 计时、key 生成、基线实现  
