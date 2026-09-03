# 04 · Size-Tiered Compaction

## 在整体中的位置

```text
WAL → MemTable → Flush → 许多小 SST → Compaction → 更少、更大的 SST
```

没有 Compaction，LSM 只完成了「顺序写」；有了 Compaction，才闭环「空间回收与读路径收敛」。

## 本系统如何挑选文件

1. 计算每个 SST 的 `SizeTierBucket(file_size)`  
2. 若某桶内文件数 ≥ `compaction_trigger`，取该桶内文件号最小的若干个（偏旧）  
3. 后台线程多路归并 → 新 SST → 更新 `tables_` 与 MANIFEST → 删除旧文件  

Flush 与 Compaction 共用一个后台线程：先刷 Immutable，再循环执行可触发的 Size-Tiered 合并。

## 归并语义

内部 key 有序（user_key 升序、sequence 降序）。归并堆弹出时：

- 若与上一条 user_key 相同 → 丢弃（更旧）  
- 否则写入输出表  

因此合并后每个 user_key 在输出中最多保留一个最新版本（含删除标记）。

## 演示

```bash
./build/test_compaction
```

日志中关注：

- `compaction_count >= 1`：确实发生了合并  
- `sst_count` 明显小于「只 Flush」时的文件数  
- 重开库后关键 key 仍可读  

对照文档：[../compaction.md](../compaction.md)
