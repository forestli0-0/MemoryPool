# MemoryPool

高性能 C++ 内存池实验项目，按版本逐步演进，从简单定长内存池到多级缓存分配器，再到更接近游戏引擎风格的 V4 分配器实现。

## 项目概览

仓库当前包含四个主要版本：

- `v1/`：哈希桶 + 定长槽位内存池，适合理解基础内存池设计。
- `v2/`：引入 `ThreadCache + CentralCache + PageCache` 的三级缓存结构。
- `v3/`：在 `v2` 基础上继续完善的跨平台版本，使用 `VirtualAlloc` / `mmap` 管理页级内存。
- `v4/`：更接近 UE-Lite Binned 思路的版本，采用固定小对象 size class、线程缓存、中心缓存和页分配器协作。

## 各版本特点

### V1

- 以 `8` 字节为粒度，将 `8` 到 `512` 字节的请求映射到 `64` 个定长内存池。
- 支持从空闲链表中以 `O(1)` 复杂度分配和回收小对象。
- 提供 `newElement` / `deleteElement` 这类模板接口，便于直接承载对象构造和析构。
- 代码结构简单，适合学习内存池的基本组成：Block、Slot、FreeList、桶映射。

![V1 架构图](images/v1.jpg)

### V2 / V3

- 引入三级缓存架构：`ThreadCache -> CentralCache -> PageCache`。
- `ThreadCache` 使用线程私有缓存，降低多线程分配时的锁竞争。
- `CentralCache` 负责批量分配与回收，改善线程间共享内存的复用效率。
- `PageCache` 负责向操作系统申请和归还大块页内存，并处理页级切分与合并。
- `v3` 针对平台兼容性做了进一步实现，使用 `VirtualAlloc` / `mmap` 管理底层内存。

![V2/V3 架构图](images/v2.png)

### V4

- 位于 `v4/`，是当前仓库里功能最完整的版本。
- 小对象固定 size class 覆盖到 `1024` 字节。
- 采用 `ThreadCache + CentralCache + PageAllocator` 三层结构。
- 支持页级 split / coalesce，以及向操作系统归还内存。
- 提供统计接口、主动回收接口以及更完整的测试和场景基准程序。

## 仓库结构

```text
memory-pool/
├── v1/                  # 基础定长内存池版本
├── v2/                  # 三级缓存版本
├── v3/                  # 跨平台优化版三级缓存
├── v4/                  # UE-Lite Binned 风格版本
├── benchmarks/          # 统一基准测试
├── images/              # README 配图
└── README.md
```

## 构建说明

每个版本目录都带有独立的 `CMakeLists.txt`，可以分别构建。

### 通用构建方式

以 `v4` 为例：

```bash
cmake -S v4 -B v4/build -DCMAKE_BUILD_TYPE=Release
cmake --build v4/build --config Release
```

如果要构建其它版本，只需要把 `v4` 替换为 `v1`、`v2` 或 `v3`。

### Windows 常见方式

仓库中已经保留了一些历史构建目录，例如：

- `v1/build/`
- `v2/build/`
- `v3/build/`
- `v4/build-ucrt64/`
- `benchmarks/build-ucrt64/`

如果你使用 MSYS2 UCRT64 + Ninja，也可以继续沿用这些目录。

## 运行说明

### V1

`v1` 的 CMake 目标会生成单个可执行文件：

```bash
./MemoryPoolProject
```

Windows 下通常对应：

```bash
./MemoryPoolProject.exe
```

### V2 / V3

这两个版本通常会生成：

```bash
./unit_test
./perf_test
```

Windows 下通常对应：

```bash
./unit_test.exe
./perf_test.exe
```

### V4

`v4` 目录默认会生成三个目标：

```bash
./unit_test
./perf_test
./scenario_bench
```

Windows 下通常对应：

```bash
./unit_test.exe
./perf_test.exe
./scenario_bench.exe
```

## 基准测试

统一基准相关文件位于 `benchmarks/`：

- `benchmarks/src/RunV1.cpp`
- `benchmarks/src/RunV3.cpp`
- `benchmarks/src/RunV4.cpp`
- `benchmarks/run_unified_bench.ps1`

已有构建输出和 CSV 结果保存在 `build/bench/` 以及 `benchmarks/build-ucrt64/` 下，可用于不同版本间的对比分析。

## 备注

个人学习资料与私有笔记仅保留在本地环境，不纳入远程仓库。

## 技术栈

- C++11 / C++17
- CMake
- 多线程编程
- 原子操作与无锁结构
- 页级内存管理
- Windows `VirtualAlloc` / Linux `mmap`

## License

MIT License
