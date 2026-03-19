# V4 Memory Pool

V4 是当前仓库里功能最完整的一版分配器实现，核心结构为：

- `ThreadCache`：线程本地小对象缓存，减少高频分配释放时的锁竞争
- `CentralCache`：按 `size class` 维护 pool，负责批量获取、批量回收和空池下放
- `PageAllocator`：按页管理 span，负责 split、coalesce 以及向 OS 申请和释放内存

## 目录结构

```text
v4/
├── include/
│   ├── Common.h
│   ├── MemoryPool.h
│   ├── ThreadCache.h
│   ├── CentralCache.h
│   └── PageAllocator.h
├── src/
│   ├── ThreadCache.cpp
│   ├── CentralCache.cpp
│   └── PageAllocator.cpp
├── tests/
│   ├── UnitTest.cpp
│   ├── PerformanceTest.cpp
│   ├── ScenarioBenchmark.cpp
│   └── TuningSweep.cpp
└── CMakeLists.txt
```

## 设计要点

- 小对象范围为 `8~1024B`，使用固定 `size class`
- 大于 `1024B` 的请求直接走页级分配路径
- 单个 pool 固定占用 `16` 页
- 小对象释放路径为 `ThreadCache -> CentralCache -> PageAllocator`
- `scavenge()` 用于主动下推缓存，最终尽量把空闲内存归还给 OS

## 构建

```bash
cmake -S v4 -B v4/build -DCMAKE_BUILD_TYPE=Release
cmake --build v4/build --config Release
```

如果使用 MSYS2 UCRT64 + Ninja，也可以单独指定生成器和编译器。

## 可执行目标

- `unit_test`：功能与状态转换测试
- `perf_test`：基础性能对比，其中多线程段采用 steady-state 计时，不包含线程退出阶段的 per-thread cache 下推成本
- `scenario_bench`：场景化 workload benchmark
- `tuning_sweep`：参数扫描工具，固定跑 steady-state / 关键场景并输出不同 tuning 组合的结果对比

## 本地测试结果

以下结果来自本地 Windows 环境下使用 `MSYS2 UCRT64 + g++ + Ninja` 的一次重新构建与测试，时间数据取 `perf_test` 和 `scenario_bench` 各 `3` 轮平均值，仅作为样例参考。

其中 `perf_test` 的多线程段采用 steady-state 口径：线程先统一起跑，循环完成后立即停表，再允许线程退出，从而避免把线程结束阶段的缓存下推成本混入分配热路径时间。当前 Windows / MinGW 下的线程缓存对象通过 OS TLS 回调清理，避免依赖 C++ `thread_local` 析构顺序。

### 功能测试

- `unit_test`：通过

### 基础性能测试

| 场景 | V4 平均耗时 | `new/delete` 平均耗时 | 结果 |
| --- | ---: | ---: | --- |
| Single Thread | `7.620 ms` | `8.408 ms` | V4 略快，约 `1.10x` |
| Multi Thread (Steady State) | `4.879 ms` | `5.284 ms` | V4 略快，约 `1.08x` |
| Mixed Size | `4.457 ms` | `5.715 ms` | V4 更快，约 `1.28x` |
| Comparable Shape | `152.363 ms` | `157.385 ms` | V4 略快，约 `1.03x` |

### 场景化 Benchmark

| 场景 | V4 平均耗时 | `new/delete` 平均耗时 | 结果 |
| --- | ---: | ---: | --- |
| Frame Small Churn | `22.250 ms` | `79.721 ms` | V4 更快，约 `3.58x` |
| Burst And Reuse | `12.312 ms` | `25.091 ms` | V4 更快，约 `2.04x` |
| Cross Thread Handoff | `12.181 ms` | `11.665 ms` | 基本持平，V4 略慢 |
| Scene Switch And Scavenge | `9.106 ms` | `11.311 ms` | V4 更快，约 `1.24x` |
| Mixed Realistic | `50.533 ms` | `51.781 ms` | 基本持平，V4 略快 |

### 结果观察

- 单线程以及明显可复用的小对象场景下，三级缓存结构可以稳定降低分配开销。
- 在剥离线程退出阶段的 TLS flush 成本后，多线程 steady-state 小对象分配已经可以接近甚至局部反超 `new/delete`，但跨线程交接仍然是最顽固的慢路径。
- 当前默认 tuning 来自 `frontier` 扫描加 steady-state 守门场景的综合评分，而不是只看单个热点场景。
- 这组默认值把 `Cross Thread Handoff` 的 `central acquire/release` 压到了约 `1.4k / 0.3k`，把 `Mixed Realistic` 的中央层往返压到了约 `77 / 7`，说明小对象路径的批处理开销已经明显下降。
- 将页层自动 trim 从“每次大对象释放都尝试”改成“明显高水位后再触发”后，`Mixed Realistic` 的 `page cache_hit` 明显升高，`sys_a` 通常稳定在约 `542` 次，workload 阶段基本不再发生 `sys_f`。
- 仅用 `Cross Thread Handoff + Mixed Realistic` 排名会把某些 steady-state 退化漏掉，因此 `tuning_sweep` 现在会把 steady-state 多线程段一起计分，默认值更偏向综合平衡而不是只冲某个单场景极值。
- 上述优化的代价是 workload 结束后 `cached_pages / freeable_pages` 往往更高，说明更多回收被推迟到了显式 `scavenge()` 阶段，这是一种用更高峰值缓存换取更低热路径开销的策略。
- `scenario_bench` 中配合 `getStats()` 观察到，workload 结束后内存通常会先停留在线程层、中央层或页层缓存中，而 `scavenge()` 后可继续下推并回收给 OS。

## 状态统计

`MemoryPool::getStats()` 现在不仅返回整体状态，还会返回三层运行时计数，便于把瓶颈拆到 `ThreadCache / CentralCache / PageAllocator`。

## 参数调优

当前版本已经把一批关键启发式参数抽成了运行时 tuning，可通过 `MemoryPool::applyTuning()` / `MemoryPool::resetTuning()` 切换，主要包括：

- batch 目标字节数与 batch 上限
- `ThreadCache` 的保留 batch 数与高水位 batch 数
- `CentralCache` 各尺寸段的空池保留数量
- `PageAllocator` 的页缓存高低水位与自动 trim 触发阈值

当前默认 tuning 为：

- batch `24KB / max 96`
- `ThreadCache` 水位 `retain 4x batch / high-water 8x batch`
- `CentralCache` 空池保留 `8 / 4 / 2`
- `PageAllocator` 页水位 `1024 / 512`，自动 trim 触发 `4096 pages`

建议优先用 `tuning_sweep` 做批量比较，而不是直接改常量重编译。默认会扫描几组常见预设，也可以继续往 `tests/TuningSweep.cpp` 里追加组合。当前推荐先跑 `grid` 或 `frontier`，并配合 `--csv` 保存完整结果。

```bash
cmake --build v4/build --target tuning_sweep
./v4/build/tuning_sweep 3
```

末尾参数表示每组预设跑几轮平均，默认值为 `3`。

如果要跑小网格扫描并把结果落盘成 CSV，可以使用：

```bash
./v4/build/tuning_sweep --runs 1 --mode grid --top 10 --csv v4/build/tuning-grid.csv
```

如果要围绕当前较优参数区间继续扩大搜索，可以使用：

```bash
./v4/build/tuning_sweep --runs 3 --mode frontier --compact --top 12 --csv v4/build/tuning-frontier.csv
```

- `--mode presets`：只跑手工挑选的少量预设
- `--mode grid`：跑一组小网格参数组合
- `--mode frontier`：围绕当前较优参数区间做更激进的一轮扫描
- `--compact`：每组参数只打印一行摘要，便于大批量扫描时快速看排名趋势
- `--csv PATH`：把完整结果写到 CSV，方便后续筛选和画图
- `--top N`：只打印排名前 `N` 的组合

### 整体状态字段

- `active_pool_count`：中央层当前仍在使用中的 pool 数量
- `empty_pool_count`：中央层已清空、但尚未继续下推的 pool 数量
- `cached_free_pages`：页层缓存中的空闲页数量
- `page_allocator.immediately_freeable_pages`：当前可以立即释放给 OS 的整 reservation 页数量，在 `scenario_bench` 的状态行中显示为 `freeable_pages`
- `os_reserved_bytes`：累计向 OS 申请的字节数
- `os_released_bytes`：累计归还给 OS 的字节数

这些字段主要用于判断内存在 workload 结束后停留在线程层、中央层还是页层，以及 `scavenge()` 后是否最终回收到 OS。

### ThreadCache 统计

`scenario_bench` 会打印 `thread workload` 和 `thread scavenge` 两行，核心字段包括：

- `hit` / `miss`：线程本地分配命中与未命中次数
- `fetch` / `fetched_blk`：从中央层批量拉取次数与块数
- `return` / `returned_blk`：向中央层批量归还次数与块数
- `flushed_blk`：线程缓存在线程结束或显式 `scavenge()` 时下推的块数
- `large_a` / `large_f`：绕过小对象路径、直接走页层的大对象申请与释放次数

### CentralCache 统计

`scenario_bench` 会打印 `central workload` 和 `central scavenge` 两行，核心字段包括：

- `acquire` / `release`：中央层批量获取与批量归还调用次数
- `blk_out` / `blk_in`：中央层向线程层发放、从线程层收回的块数
- `partial` / `empty`：优先命中 partial pool 还是 empty pool
- `create` / `release_pool`：中央层 pool 的创建与释放次数
- `scav`：中央层显式回收次数

### PageAllocator 统计

`scenario_bench` 会打印 `page workload` 和 `page scavenge` 两行，核心字段包括：

- `span_a` / `span_f`：页级 span 的申请与释放次数
- `cache_hit` / `cache_miss`：页层缓存命中与未命中次数
- `sys_a` / `sys_f`：真正触发 OS 申请与释放的次数
- `merge`：页级 coalesce 合并次数
- `released`：最终释放回 OS 的整 span 数量
- `scavenge`：页层回收逻辑调用次数

## 分析方法

建议每次优化都固定按下面的顺序分析：

1. 先看 `V4 workload` 与 `new/delete` 的总时间差，确认慢的是 steady-state 还是 `scavenge()`。
2. 再看 `thread workload / central workload / page workload` 三行，判断时间主要花在哪一层。
3. 再额外对照 steady-state 多线程段，确认某组 tuning 没有为了压低 `Cross Thread Handoff` 而明显伤到常规同线程热路径。
4. 最后对照 `after workload` 与 `after scavenge` 的状态字段，确认空闲对象最终停留在哪一层缓存。

### 常见判读模式

- `ThreadCache miss` 很高，但 `page workload` 很低：
  说明热点更像是线程缓存容量或 batch 策略不合适，优先调 `batch count`、回收阈值或 TLS 缓存策略。
- `central create`、`page span_a`、`page cache_miss` 同时升高：
  说明中央层无法复用已有 pool，问题更可能在跨线程 free、pool churn 或 span owner 查找路径。
- `large_a` / `large_f` 很高，并且 `page sys_a` / `sys_f` 也很高：
  说明慢点主要在大对象路径，而不是小对象 TLS fast path。
- `after workload` 的 `freeable_pages` 很高，但 `after scavenge` 能快速归零：
  说明 workload 结束后已经形成大量可立即回收的整 reservation，trim 策略是有效的。
- `os_released_bytes` 在 workload 阶段就大幅上涨：
  说明回收策略过于激进，热路径里发生了明显的页级抖动。

### 当前版本的典型特征

- `Cross Thread Handoff` 往往表现为 `ThreadCache miss` 不是特别高，但 `central create`、`page span_a`、`page cache_miss` 会明显升高，说明瓶颈主要在跨线程归还后的中央层和页层扩张路径。
- `Mixed Realistic` 往往伴随大量 `large_a` / `large_f`，同时 `page cache_hit`、`cache_miss`、`sys_a` 都很活跃，说明这类负载会部分退化为“在测大对象页分配器”。
- `Frame Small Churn` 和 `Burst And Reuse` 通常表现为高 `ThreadCache hit`、低 `page workload`，这是 V4 最能发挥优势的场景。

## 建议流程

后续每次优化建议都按同一套流程执行：

1. 跑 `unit_test`，先保证行为不回退。
2. 跑 `scenario_bench`，保存三层统计输出。
3. 跑 `tuning_sweep --mode grid` 或 `--mode frontier`，至少保留一份 CSV。
4. 先分析 `Cross Thread Handoff` 和 `Mixed Realistic`，但同时检查 steady-state 守门场景是否被拉坏。
5. 只有在三层统计明确指向某个瓶颈后，再针对该路径改数据结构或回收策略。
