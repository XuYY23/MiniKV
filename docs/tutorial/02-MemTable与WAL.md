# 02 · MemTable 与 WAL

## 写路径

```text
DB::Put / Delete
  → 编码为 WriteBatch（带 sequence）
  → WalWriter::AddRecord（先落日志）
  → 根据写选项执行 fsync
  → WriteBatch::InsertInto(MemTable)
  → 更新 last_sequence_
```

先 WAL 再 MemTable：进程在 MemTable 更新后、刷盘前崩溃时，仍可通过日志找回已成功返回的写入。

## WriteBatch

一条 WAL 记录对应一个 `WriteBatch`：

```text
[sequence:8][count:4][records...]
record = tag + len-prefixed key [+ len-prefixed value]
tag = kTypeValue | kTypeDeletion
```

批量编码按「一批」提交到日志；恢复时按同样格式回放。当前 API 每次 Put/Delete 构成单记录 batch，格式支持多记录批量。

## WAL 物理格式

```text
[checksum:4][length:4][payload]
```

- checksum：对 `length||payload` 做 CRC-32C，再 mask（避免嵌入式 CRC 与长度字段偶发对齐问题）  
- 读到**文件末尾的截断记录**视为正常结束（崩溃可能砍在半条记录上）  
- **中间记录校验失败**视为 Corruption  

对应实现：`src/engine/wal.*`，`tests/engine/test_wal.cpp`。

## MemTable 条目格式

```text
key_len | user_key | tag(8) | value_len | value
tag = (sequence << 8) | type
```

`Get` 用 `LookupKey(user_key, last_sequence)` 构造查找键，在跳表中 `Seek`，检查用户 key 是否匹配，再解释 type。

## 恢复

`DBImpl::Recover`：

1. 从 MANIFEST 读取 current WAL 和 previous WAL 编号  
2. 顺序读取仍受保护的日志记录  
3. 每个 payload 还原为 `WriteBatch` 并写入 MemTable  
4. 推进 `last_sequence_`  
5. previous WAL 存在时，先完成恢复 Flush，再对外提供写服务  

`tests/engine/test_db.cpp` 覆盖正常关闭后的恢复；`tests/engine/test_crash_recovery.cpp` 使用子进程 `_exit` 绕过析构与 `Close`，覆盖 active/immutable MemTable 跨崩溃恢复。

## 手动演示

```bash
mkdir -p data/demo
./build/minikv_cli ./data/demo put a 1
./build/minikv_cli ./data/demo put b 2
./build/minikv_cli ./data/demo delete a
./build/minikv_cli ./data/demo get a    # (null)
./build/minikv_cli ./data/demo get b    # 2
# 每次 CLI 都是 Open→操作→Close，第二次 get 从持久化状态恢复
```

### 把 `.log` 读成可读输出

WAL 是二进制格式，可用 dump 工具解码：

```bash
./build/minikv_wal_dump ./data/demo/000001.log
```

会按记录打印 `seq` / `Put` / `Delete`。单元测试：`./build/test_wal_dump`。

## 与 Flush 的衔接

当 `ApproximateMemoryUsage()` 超过 `write_buffer_size` 时：

1. 当前 MemTable 变为 Immutable  
2. 新建活跃 MemTable 继续写入  
3. 后台 Flush 把 Immutable 写成 SSTable  
4. MANIFEST 同时记录 previous WAL 与 current WAL  
5. SST 安装进 MANIFEST 后删除 previous WAL  

详见 [03-Flush与SSTable](03-Flush与SSTable.md)。
