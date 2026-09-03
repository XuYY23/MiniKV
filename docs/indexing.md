# 内存索引选型（MemTable）

## 问题

MemTable 保存尚未刷盘的最新数据，需要支持：

- 按用户 key 有序存放（刷盘时可顺序写出）  
- 高效插入与点查  
- 同一 key 多版本（靠 sequence 区分新旧）  

## 方案对比

| 结构 | 查询/插入 | 优势 | 代价与适用边界 |
|------|-----------|------|-----------------|
| **SkipList** | 期望 O(log n) | 天然有序；迭代简单；节点只增不改时并发控制边界清楚 | 指针和高层索引占空间；复杂度是期望值而非严格上界 |
| 红黑树 / AVL | O(log n) | 最坏复杂度有界；标准库通常已有成熟实现 | 旋转会修改多处指针；Arena 上的一次性节点管理不如跳表直接 |
| 哈希表 | 平均 O(1) 点查 | 等值查询路径短 | 无序；Flush 前必须排序，峰值内存和刷盘延迟上升 |
| Radix Tree / ART | 与 key 长度相关 | 共享前缀；字符串 key 上可兼顾点查和有序遍历 | 节点类型和路径压缩较复杂；长公共前缀与随机 key 的表现不同 |

## MiniKV 的选择

**采用 SkipList。**

节点分配在 `Arena` 上：只分配、随表销毁一次性回收，避免频繁 `new/delete`。

比较器使用 **internal key**：

```text
user_key 升序，sequence 降序（更新的版本更靠前）
```

`Get` 时 `Seek` 到第一个用户 key 匹配的条目即可看到最新版本；若类型为删除标记，则对用户表现为 NotFound。

红黑树提供严格的 O(log n) 上界，但它没有改变本项目最重要的刷盘能力；哈希表虽然缩短点查路径，却会把排序成本推迟到 Flush。SkipList 直接产出有序迭代流，因此 Flush 不需要额外复制和排序整个 MemTable，这是最终采用它的主要原因。

## 相关代码

- `src/engine/skiplist.h` — 插入、查找、迭代  
- `src/engine/arena.*` — 块分配  
- `src/engine/dbformat.*` — internal key / LookupKey  
- `src/engine/memtable.*` — 封装 SkipList 的表语义  
- `tests/engine/test_skiplist.cpp` / `tests/engine/test_memtable.cpp`  
