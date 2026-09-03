# 09 · Raft 日志复制与提交

在选主之上补齐：内存日志、`AppendEntries` 复制、`commitIndex` 多数派推进、`Apply` 回调。

## 写路径（Leader）

```text
Propose(cmd)
  → 追加 (term, cmd) 到本地 log
  → 向 peer 发 AppendEntries（含 nextIndex 起的条目）
  → 根据 matchIndex 统计多数派
  → 仅提交「当前任期」已达多数的条目（Raft Figure 8）
  → lastApplied 追上 commitIndex 时调用 StateMachine::Apply
  → 目标日志已提交后 Propose 返回成功
```

Leader 只提交当前任期达到法定多数派的条目。新 Leader 在首次一致性读取前追加并提交当前任期 no-op；该屏障使更早任期的已提交前缀先完成 Apply，再读取本地状态机。

## Follower

- 校验 `prevLogIndex/prevLogTerm`  
- 冲突则截断后缀再追加  
- `leaderCommit` 推进本地 `commitIndex` 并 Apply  
- 日志持久化失败时 AppendEntries 返回失败  

## 测试

| 用例 | 目的 |
|------|------|
| basic Propose/commit/Apply | 主路径 |
| 冲突后缀覆盖 | 旧 Leader 残留日志被截断 |
| Figure 8 | 旧任期条目不能单独提交 |
| 日志新旧投票 | 过期 Candidate 被拒 |
| 分区后追赶 | Unregister 再 Register 追上 commit |
| 少数派 Leader | Propose 与领导权确认均失败 |
| 新任期读屏障 | 多数派确认和当前任期提交共同约束读取 |

```bash
./build/test_raft_replicate
```

## 快照与成员变更

`CreateSnapshot` 把 `lastApplied` 之前的状态交给状态机序列化，并把日志压缩为“快照基准索引 + 后缀”。Leader 发现 `nextIndex` 已落到压缩边界之前时发送 `InstallSnapshot`，成功后继续发送剩余日志。内存传输和 TCP 传输使用相同协议结构。

成员变更使用两阶段 joint consensus：

1. 新副本先追赶现有日志；
2. 提交 `C_old,new`，每次提交和选举都同时满足旧集合、新集合的多数派；
3. 在联合配置下提交 `C_new`，结束联合阶段。

对应测试：`test_raft_snapshot`、`test_raft_membership`、`test_raft_tcp`。
