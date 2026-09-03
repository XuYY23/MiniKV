# 10 · Raft 持久化与 Apply 接引擎

## 持久化

相对目录（如 `testdata/.../raft1/`）：

| 文件 | 内容 |
|------|------|
| `hard_state` | term / votedFor / commitIndex / lastApplied |
| `raft.log` | 快照基准索引、基准任期及其后的日志条目 |
| `snapshot` | lastIncludedIndex / lastIncludedTerm / 成员配置 / 状态机数据 |

`RaftStorageInterface` 定义加载完整持久化状态、保存日志、保存 hard state 和保存快照的接口，`RaftStorage` 提供文件实现。文件包含 CRC-32C 校验；写入采用临时文件、fsync、rename 和目录 fsync。持久化失败会沿 RPC 或 Propose 返回，不计入法定多数派。读取器仍兼容 HST1/LOG1 与 LOG2；新写入使用 HST2/LOG3/SNP1。

## Apply → LSM

```text
Propose(KvCommand::EncodePut/Delete)
  → 复制并 commit
  → StateMachine::Apply → KvStateMachine → DB::Put/Delete
```

`StateMachine::Apply` 返回执行状态并满足幂等语义。`CreateSnapshot` 导出状态，`RestoreSnapshot` 在恢复或接收快照时重建状态。KV 状态机使用同步 WAL；Apply 成功后 Raft 才推进并持久化 `lastApplied`。启动时先恢复快照，再重放其后的已提交日志，可重建空状态机目录。

## 测试

```bash
./build/test_raft_persist
```

覆盖：重启恢复 hard_state/log、Apply 进 MiniKV、持久化错误拒绝确认、坏 magic 与非法索引拒绝。`test_raft_snapshot` 另行覆盖快照持久化、日志截断、重启恢复和落后副本安装快照。

下一节：静态分片与每分片一组 Raft。
