# Compaction 策略对比与选型

## 问题

Flush 会不断产生小 SSTable。若不合并：

- 文件数膨胀，打开与点查成本上升  
- 同一 key 的旧版本 / 删除标记长期占用空间  

Compaction 把多份 SST 归并，丢掉被更新覆盖的旧版本，换取更少的文件与更干净的读路径。

## 两种主流策略

| | Size-Tiered | Leveled |
|--|-------------|--------|
| 文件组织 | 大小相近的若干 SST 合并成更大的 SST；同一层的 key 范围允许重叠 | L0 允许重叠，L1 以后同层文件的 key 范围通常不重叠 |
| 触发条件 | 同一大小桶的文件数达到阈值 | 某层文件总量超过容量预算，选择它与下一层重叠文件 |
| 写放大 | 通常较低，一个文件达到下一尺寸前合并次数较少 | 通常较高，同一数据会随层级增长多次重写 |
| 读放大 | 较高，点查可能检查多个重叠 SST | 较低，除 L0 外每层通常至多命中一个文件 |
| 空间放大 | 合并较晚，旧版本存留较久 | 旧版本更快进入合并，但合并期间需要输入与输出共存 |
| 适用负载 | 写吞吐优先、能够容忍更多读 IO | 点查和范围读延迟稳定性优先 |

## MiniKV 的选择

**实现 Size-Tiered Compaction。**

具体规则：

1. 用 `log2` 风格把 `file_size` 分到大小桶  
2. 某一桶内文件数 ≥ `Options::compaction_trigger`（默认 4）时触发  
3. 取该桶内最旧的 `trigger` 个文件做多路归并  
4. 归并时同一 `user_key` 只保留最新 internal key  
5. 输出 SST 同步落盘后，先用原子替换的 MANIFEST 安装新版本，再删除输入文件  

选择 Size-Tiered 的原因：与「Flush 产生一批相近大小文件」的形态贴合，触发条件与效果（文件数下降、空间回收）更容易观察。Leveled 的机制与权衡见上表；本仓库不并行实现第二套合并器。

SST 的文件号不能直接代表其中每个 key 的新旧关系。Compaction 输出可能由旧文件生成，而另一个未参与合并的文件包含更新版本。读取路径因此比较候选 internal key 的 sequence，选择 sequence 最大的记录，而不是简单选择最后生成的 SST。

## 相关代码

- `src/engine/compaction.*` — 挑选与归并  
- `src/engine/db_impl.*` — 后台在 Flush 后继续跑 Compaction  
- `tests/engine/test_compaction.cpp` — 验证合并发生且数据仍正确  

## 参数

| 选项 | 含义 | 默认 |
|------|------|------|
| `compaction_trigger` | 同桶最少文件数 | 4 |
| `disable_auto_compaction` | 关闭自动合并 | false |
