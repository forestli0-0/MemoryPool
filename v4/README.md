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
│   └── ScenarioBenchmark.cpp
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
- `perf_test`：基础性能对比，其中多线程段采用 steady-state 计时，不包含线程退出阶段的 TLS flush 成本
- `scenario_bench`：场景化 workload benchmark

## 本地测试结果

以下结果来自本地 Windows 环境下使用 `MSYS2 UCRT64 + g++ + Ninja` 的一次重新构建与测试，时间数据取 `perf_test` 和 `scenario_bench` 各 `5` 轮平均值，仅作为样例参考。

其中 `perf_test` 的多线程段采用 steady-state 口径：线程先统一起跑，循环完成后立即停表，再允许线程退出，从而避免把 `thread_local ThreadCache` 在线程退出时的缓存下推成本混入分配热路径时间。

### 功能测试

- `unit_test`：通过

### 基础性能测试

| 场景 | V4 平均耗时 | `new/delete` 平均耗时 | 结果 |
| --- | ---: | ---: | --- |
| Single Thread | `2.255 ms` | `7.547 ms` | V4 更快，约 `3.35x` |
| Multi Thread (Steady State) | `4.323 ms` | `4.231 ms` | 基本持平，V4 略慢 |
| Mixed Size | `5.514 ms` | `6.334 ms` | V4 略快，约 `1.15x` |
| Comparable Shape | `48.436 ms` | `149.340 ms` | V4 更快，约 `3.08x` |

### 场景化 Benchmark

| 场景 | V4 平均耗时 | `new/delete` 平均耗时 | 结果 |
| --- | ---: | ---: | --- |
| Frame Small Churn | `29.631 ms` | `73.280 ms` | V4 更快，约 `2.47x` |
| Burst And Reuse | `15.635 ms` | `23.429 ms` | V4 更快，约 `1.50x` |
| Cross Thread Handoff | `21.046 ms` | `11.217 ms` | V4 更慢，约 `1.88x` |
| Scene Switch And Scavenge | `9.411 ms` | `8.880 ms` | 基本持平，V4 略慢 |
| Mixed Realistic | `67.971 ms` | `43.189 ms` | V4 更慢，约 `1.57x` |

### 结果观察

- 单线程以及明显可复用的小对象场景下，三级缓存结构可以稳定降低分配开销。
- 在剥离线程退出阶段的 TLS flush 成本后，多线程 steady-state 小对象分配已经接近 `new/delete`，但中央层和页层慢路径的常数项仍然偏高。
- 跨线程交接和更复杂的混合负载下，当前实现仍有进一步优化空间。
- `scenario_bench` 中配合 `getStats()` 观察到，workload 结束后内存通常会先停留在线程层、中央层或页层缓存中，而 `scavenge()` 后可继续下推并回收给 OS。

## 状态统计

`MemoryPool::getStats()` 会返回以下关键信息：

- `active_pool_count`
- `empty_pool_count`
- `cached_free_pages`
- `os_reserved_bytes`
- `os_released_bytes`

这些字段主要用于观察对象在 workload 结束后停留在线程层、中央层还是页层，以及 `scavenge()` 后是否最终归还给 OS。
