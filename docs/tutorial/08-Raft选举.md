# 08 · Raft 选举

在 `src/raft/` 实现选主主路径。

## 角色与流程

```text
Follower --(选举超时)--> Candidate --(获多数票)--> Leader
                ^                |
                +--(发现更高 term / 收到合法心跳)--+
```

- `RequestVote`：term / candidate_id / last_log_*  
- Leader 周期性发空 `AppendEntries` 作心跳，Follower 重置选举计时  
- 内存状态与持久化状态使用相同的选举规则；持久化机制见第 10 章  

## 传输

| 实现 | 用途 |
|------|------|
| `MemoryTransport` | 确定性单元测试中的进程内直调 |
| `TcpRaftTransport` | 根据节点地址表发送 RequestVote、AppendEntries 和 InstallSnapshot |
| `HandleRaftRpcPayload` | 把 TCP 请求分派到目标 RaftNode |

## 运行测试

```bash
./build/test_raft_election
```
