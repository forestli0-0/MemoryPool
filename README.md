# MemoryPool
高性能C++内存池实现项目

> 基于C++实现的自定义内存池框架，包含V1单版本和V2/V3多级缓存架构两个主要版本

## 项目介绍

本项目是基于C++实现的自定义内存池框架，旨在提高内存分配和释放的效率，特别是在多线程环境下。
该项目中实现的内存池主要分为两个版本，分别是目录v1、v2和v3（v3在v2基础上进行了平台兼容性优化），这两个版本的内存池设计思路大不相同。

### v1 介绍
基于哈希映射的多种定长内存分配器，可用于替换new和delete等内存申请释放的系统调用。包含以下主要功能：
- 内存分配：提供allocate方法，从内存池中分配内存块
- 内存释放：提供deallocate方法，将内存块归还到内存池
- 内存块管理：通过allocateNewBlock方法管理内存块的分配和释放
- 自由链表：使用无锁的自由链表管理空闲内存块，提高并发性能

项目架构图如下：
![alt text](images/v1.jpg)

### v2、v3 介绍
该项目包括以下主要功能：
- 线程本地缓存（ThreadCache）：每个线程维护自己的内存块链表，减少线程间的锁竞争，提高内存分配效率
- 中心缓存（CentralCache）：用于管理多个线程共享的内存块，支持批量分配和回收，优化内存利用率
- 页面缓存（PageCache）：负责从操作系统申请和释放大块内存，支持内存块的合并和分割，减少内存碎片
- 自旋锁和原子操作：在多线程环境下使用自旋锁和原子操作，确保线程安全的同时减少锁的开销

项目架构图如下：
![alt text](images/v2.png)

## 技术亮点

### V1版本
- 基于哈希映射的64个定长内存池（8-512字节）
- 无锁自由链表实现（CAS原子操作）
- O(1)时间复杂度的内存分配
- 单线程性能比new/delete快65%
- 适合理解内存池基础概念

### V3版本（三级缓存架构）
- ThreadCache：线程级私有缓存，0锁竞争
- CentralCache：全局共享缓存，批量分配回收
- PageCache：页级缓存，减少系统调用
- Windows/Linux跨平台支持（VirtualAlloc/mmap）
- 单线程性能比new/delete快7.6倍
- 多线程性能比new/delete快9.5倍

## 编译

先进入v1或v2或v3项目目录：
```bash
cd v1  # 或 cd v2 / cd v3
```

在项目目录下创建build目录，并进入该目录：
```bash
mkdir build
cd build
```

执行cmake命令：
```bash
cmake ..
```

执行make命令：
```bash
make
```

删除编译生成的可执行文件：
```bash
make clean
```

## 运行

Windows:
```bash
cd build/Release
./unit_test.exe    # 单元测试
./perf_test.exe    # 性能测试
```

Linux/macOS:
```bash
./unit_test    # 单元测试
./perf_test    # 性能测试
```

## 项目结构

```
memory-pool/
├── v1/                    # 单版本哈希映射内存池
│   ├── include/
│   │   └── MemoryPool.h   # 核心实现
│   ├── src/
│   │   └── MemoryPool.cpp
│   └── tests/
│       └── UnitTest.cpp
│
├── v2/                    # 三级缓存架构（多平台）
│   ├── include/
│   │   ├── MemoryPool.h   # 统一接口
│   │   ├── ThreadCache.h  # 线程缓存
│   │   ├── CentralCache.h # 中心缓存
│   │   └── PageCache.h    # 页面缓存
│   ├── src/
│   │   ├── ThreadCache.cpp
│   │   ├── CentralCache.cpp
│   │   └── PageCache.cpp
│   └── tests/
│       ├── UnitTest.cpp
│       └── PerformanceTest.cpp
│
├── v3/                    # 三级缓存架构（Windows优化）
│   └── ...                # 与v2类似，使用VirtualAlloc
│
└── README.md
```

## 性能测试结果

### v1
- 单线程：比new/delete快65%
- 多线程（4线程）：比new/delete慢4倍

### v3
- 单线程：比new/delete快7.6倍
- 多线程（4线程）：比new/delete快9.5倍

## 技术栈

- C++11/14
- CMake
- 多线程编程
- 内存管理（VirtualAlloc/mmap）
- 原子操作与无锁编程

## License

MIT License
