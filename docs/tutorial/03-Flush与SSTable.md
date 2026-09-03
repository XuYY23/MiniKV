# 03 · Flush 与 SSTable

## 这一节解决什么问题

只靠 WAL + MemTable 时，内存会无限涨，重启也要整段回放日志。LSM 因此引入：

1. MemTable 达到阈值 → 变成 **Immutable**  
2. 新建活跃 MemTable，前台继续写（刷盘不堵在前台路径上）  
3. 后台把 Immutable **Flush** 成不可变 **SSTable**  
4. 用 **MANIFEST** 记住有哪些 SST；恢复时先装 SST，再回放当前 WAL  

## 写路径（当前实现）

```text
Write
  → 若 mem 超 write_buffer_size：
        若已有 imm：等待后台刷完（背压）
        否则 mem → imm，换新 mem，唤醒后台线程
  → 追加 WAL
  → 写入活跃 mem

后台线程
  → 遍历 imm，TableBuilder 写出 *.sst
  → fsync SST
  → 原子更新 MANIFEST，安装 SST 并清除 previous_log_number
  → 删除 previous WAL
  → imm = nullptr
```

Get 顺序：`mem → imm → 所有候选 SSTable`。SST 候选按 internal sequence 比较，文件创建时间不直接决定记录的新旧。

## SSTable 文件布局

```text
[entries...][index offsets...][footer 24B]

entry  = len-prefixed internal_key + len-prefixed value
footer = index_offset | num_entries | magic("minikvst")
```

点查：对 index 做二分（按 user_key），再处理同 key 多版本 / 删除标记。

## MANIFEST

文本格式，便于阅读：

```text
MINIKV_MANIFEST_V1
next_file_number=...
last_sequence=...
log_number=...
sst=1
sst=2
```

MANIFEST 先写入临时文件并执行 fsync，再通过 `rename` 原子替换，最后 fsync 数据库目录。旧 WAL 和旧 SST 只在新 MANIFEST 持久化后删除。

## 相关代码

- `src/engine/table.*` — Builder / Reader  
- `src/engine/version_edit.*` — MANIFEST  
- `src/engine/db_impl.*` — MakeRoomForWrite / FlushImmTable / Get  
- `tests/engine/test_sstable.cpp`、`tests/engine/test_flush.cpp`

## 运行测试

```bash
cmake --build build -j
./build/test_sstable
./build/test_flush
./build/test_crash_recovery
```
