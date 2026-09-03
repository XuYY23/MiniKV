# tests 目录说明

按子系统分层：

```text
tests/
  testharness.h          # 公共断言与日志
  engine/                # 单机 LSM
  rpc/                   # RPC
  raft/                  # Raft
  cluster/               # 分片与集群
```

关键边界测试包括：

- `test_crash_recovery`：子进程 `_exit`，验证双 WAL 与后台 Flush 崩溃恢复
- `test_raft_persist`：持久化失败与状态机失败不得被确认
- `test_raft_tcp`：三节点经 TCP 完成选举、复制与提交
- `test_raft_snapshot`：日志压缩、重启恢复与 InstallSnapshot 追赶
- `test_raft_membership`：joint consensus 增删成员与配置恢复
- `test_cluster_rpc`：远程 Put/Get/Delete 全链路
- `test_failover`：Leader 切换、少数派和副本追赶

整机串测为 `test_engine_e2e` 与 `test_cluster_e2e`。
