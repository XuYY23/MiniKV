# 构建与完整验证

本文记录 MiniKV 在一台新的 Linux 机器上完成编译、测试、Sanitizer 检查和 Benchmark 的命令。仓库中的 `data/` 与 `testdata/` 保存一次完整运行后的示例数据和验收产物；测试程序会在构建目录下建立独立的 `testdata/`，不会依赖这些快照才能通过。

## 环境

- CMake 3.16 或更高版本
- 支持 C++17 的 GCC 或 Clang
- POSIX 线程与文件系统接口

## Release 构建与测试

```bash
cmake -S . -B build-audit -DCMAKE_BUILD_TYPE=Release
cmake --build build-audit -j
ctest --test-dir build-audit --output-on-failure
```

测试覆盖单机引擎、崩溃恢复、并发读写、RPC、Raft、快照、成员变更、静态分片、故障切换和远程客户端链路。

## ASan 与 UBSan

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
```

AddressSanitizer 检查越界、释放后使用等内存错误；UndefinedBehaviorSanitizer 检查整数、对齐和类型等未定义行为。

## ThreadSanitizer

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"

cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

ThreadSanitizer 需要运行环境支持其虚拟地址空间布局。若运行时在测试启动前报告 `unexpected memory mapping`，表示当前系统无法启动 TSAN，并非测试断言失败。

## Benchmark

```bash
./build-audit/minikv_bench \
  --ops 5000 --value-bytes 100 --db ./data/bench

./build-audit/minikv_cluster_bench \
  --ops 2000 --value-bytes 64 \
  --shards 4 --replicas 3 --db ./data/cluster_bench
```

`minikv_bench` 分解内存 map、简单 WAL 与完整 LSM 的组件成本。`minikv_cluster_bench` 对本地引擎、单分片 Raft 和多分片并行 Raft 使用同步持久化；Raft 传输为 `MemoryTransport`，结果不包含跨机网络延迟。

## 独立进程 Raft 集群

分别在三个终端运行：

```bash
./build-audit/minikv_node 1 ./data/n1 9101 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
./build-audit/minikv_node 2 ./data/n2 9102 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
./build-audit/minikv_node 3 ./data/n3 9103 1=127.0.0.1:9101 2=127.0.0.1:9102 3=127.0.0.1:9103
```

连接当前 Leader：

```bash
./build-audit/minikv_remote_cli 127.0.0.1 9101 put demo value
./build-audit/minikv_remote_cli 127.0.0.1 9101 get demo
```

Follower 对客户端请求返回 `not leader`，不会替代 Leader 确认操作。
