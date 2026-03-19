# MemoryPool

这个仓库当前主要维护和说明的是 `v4/`。

`v1/`、`v2/`、`v3/` 仅作为历史参考保留，它们来自别人的仓库与思路整理，不是当前主维护版本。后续使用、阅读和测试，默认都以 `v4` 为准。

## V4 简介

`v4` 是当前仓库里功能最完整的一版内存池实现，采用三层结构：

- `ThreadCache`
- `CentralCache`
- `PageAllocator`

核心特点：

- 小对象范围为 `8~1024B`
- 固定 `size class`
- 小对象走线程缓存和中央缓存
- 大对象直接走页级分配路径
- 支持页级 split / coalesce
- 支持 `scavenge()` 主动回收
- 提供运行时统计、场景 benchmark 和 tuning sweep

命名空间使用：

- `glock`

## 目录

```text
memory-pool/
├── v4/                  # 当前主版本
├── v1/                  # 历史参考
├── v2/                  # 历史参考
├── v3/                  # 历史参考
├── benchmarks/          # 基准测试相关代码
└── README.md
```

## 构建

```bash
cmake -S v4 -B v4/build -DCMAKE_BUILD_TYPE=Release
cmake --build v4/build --config Release
```

如果使用 MSYS2 UCRT64 + Ninja，也可以使用现有的 `v4/build-ucrt64/` 构建目录。

## 运行

`v4` 默认包含这些目标：

- `unit_test`
- `perf_test`
- `scenario_bench`
- `tuning_sweep`

例如：

```bash
./v4/build/unit_test
./v4/build/perf_test
./v4/build/scenario_bench
./v4/build/tuning_sweep 3
```

Windows 下对应 `.exe`。

## 性能测试

以下结果来自本地 Windows 环境下使用 `MSYS2 UCRT64 + g++ + Ninja` 的一次重新构建与测试，时间数据取 `perf_test` 和 `scenario_bench` 各 `3` 轮平均值，仅作为样例参考。

### 基础性能

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

### 简要结论

- 小对象高复用场景下，`v4` 优势明显。
- steady-state 多线程小对象分配已经可以接近甚至局部反超 `new/delete`。
- 跨线程交接仍然是当前最顽固的慢路径。
- 更完整的统计字段、调参方式和结果分析见 `v4/README.md`。

## 文档

更完整的设计、统计字段、调参方法和样例结果见：

- `v4/README.md`

## License

MIT License
