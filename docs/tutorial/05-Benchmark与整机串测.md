# 05 · 整机串测与 Benchmark

把单机引擎主链路串在一起，并给出可对比的吞吐数字。

详见：[../benchmark.md](../benchmark.md)

```bash
./build/test_engine_e2e
./build/minikv_bench --ops 3000 --db ./data/bench
```
