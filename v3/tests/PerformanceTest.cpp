#include "../include/MemoryPool.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <iomanip>
#include <thread>

using namespace Kama_memoryPool;
using namespace std::chrono;

// 计时器类
class Timer 
{
    high_resolution_clock::time_point start;
public:
    Timer() : start(high_resolution_clock::now()) {}
    
    double elapsed() 
    {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start).count() / 1000.0; // 转换为毫秒
    }
};

// 性能测试类
class PerformanceTest 
{
public:
    // 1. 系统预热
    static void warmup() 
    {
        std::cout << "Warming up memory systems...\n";
        // 使用 pair 来存储指针和对应的大小
        std::vector<std::pair<void*, size_t>> warmupPtrs;
        
        // 预热内存池
        for (int i = 0; i < 1000; ++i) 
        {
            for (size_t size : {32, 64, 128, 256, 512}) {
                void* p = MemoryPool::allocate(size);
                warmupPtrs.emplace_back(p, size);  // 存储指针和对应的大小
            }
        }
        
        // 释放预热内存
        for (const auto& [ptr, size] : warmupPtrs) 
        {
            MemoryPool::deallocate(ptr, size);  // 使用实际分配的大小进行释放
        }
        
        std::cout << "Warmup complete.\n\n";
    }

    // 2. 小对象分配测试
    static void testSmallAllocation() 
    {
        constexpr size_t NUM_ALLOCS = 100000;
        constexpr size_t SMALL_SIZE = 32;
        
        std::cout << "\nTesting small allocations (" << NUM_ALLOCS << " allocations of " 
                  << SMALL_SIZE << " bytes):" << std::endl;
        
        // 测试内存池
        {
            Timer t;
            std::vector<void*> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                ptrs.push_back(MemoryPool::allocate(SMALL_SIZE));
                
                // 模拟真实使用：部分立即释放
                if (i % 4 == 0) 
                {
                    MemoryPool::deallocate(ptrs.back(), SMALL_SIZE);
                    ptrs.pop_back();
                }
            }
            
            for (void* ptr : ptrs) 
            {
                MemoryPool::deallocate(ptr, SMALL_SIZE);
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // 测试new/delete
        {
            Timer t;
            std::vector<void*> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                ptrs.push_back(new char[SMALL_SIZE]);
                
                if (i % 4 == 0) 
                {
                    delete[] static_cast<char*>(ptrs.back());
                    ptrs.pop_back();
                }
            }
            
            for (void* ptr : ptrs) 
            {
                delete[] static_cast<char*>(ptr);
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }
    
    // 3. 多线程测试
    static void testMultiThreaded() 
    {
        constexpr size_t NUM_THREADS = 4;
        constexpr size_t ALLOCS_PER_THREAD = 25000;
        constexpr size_t MAX_SIZE = 256;
        
        std::cout << "\nTesting multi-threaded allocations (" << NUM_THREADS 
                  << " threads, " << ALLOCS_PER_THREAD << " allocations each):" 
                  << std::endl;
        
        auto threadFunc = [MAX_SIZE, ALLOCS_PER_THREAD](bool useMemPool) 
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(8, MAX_SIZE);
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(ALLOCS_PER_THREAD);
            
            for (size_t i = 0; i < ALLOCS_PER_THREAD; ++i) 
            {
                size_t size = dis(gen);
                void* ptr = useMemPool ? MemoryPool::allocate(size) 
                                     : new char[size];
                ptrs.push_back({ptr, size});
                
                // 随机释放一些内存
                if (rand() % 100 < 75) 
                {  // 75%的概率释放
                    size_t index = rand() % ptrs.size();
                    if (useMemPool) {
                        MemoryPool::deallocate(ptrs[index].first, ptrs[index].second);
                    } else {
                        delete[] static_cast<char*>(ptrs[index].first);
                    }
                    ptrs[index] = ptrs.back();
                    ptrs.pop_back();
                }
            }
            
            // 清理剩余内存
            for (const auto& [ptr, size] : ptrs) 
            {
                if (useMemPool) 
                {
                    MemoryPool::deallocate(ptr, size);
                } 
                else 
                {
                    delete[] static_cast<char*>(ptr);
                }
            }
        };
        
        // 测试内存池
        {
            Timer t;
            std::vector<std::thread> threads;
            
            for (size_t i = 0; i < NUM_THREADS; ++i) 
            {
                threads.emplace_back(threadFunc, true);
            }
            
            for (auto& thread : threads) 
            {
                thread.join();
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // 测试new/delete
        {
            Timer t;
            std::vector<std::thread> threads;
            
            for (size_t i = 0; i < NUM_THREADS; ++i) 
            {
                threads.emplace_back(threadFunc, false);
            }
            
            for (auto& thread : threads) 
            {
                thread.join();
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }

    // 4. 混合大小测试
    static void testMixedSizes() 
    {
        constexpr size_t NUM_ALLOCS = 50000;
        const size_t SIZES[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
        
        std::cout << "\nTesting mixed size allocations (" << NUM_ALLOCS 
                  << " allocations):" << std::endl;
        
        // 测试内存池
        {
            Timer t;
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                size_t size = SIZES[rand() % 8];
                void* p = MemoryPool::allocate(size);
                ptrs.emplace_back(p, size);
                
                // 批量释放
                if (i % 100 == 0 && !ptrs.empty()) 
                {
                    size_t releaseCount = std::min(ptrs.size(), size_t(20));
                    for (size_t j = 0; j < releaseCount; ++j) 
                    {
                        MemoryPool::deallocate(ptrs.back().first, ptrs.back().second);
                        ptrs.pop_back();
                    }
                }
            }
            
            for (const auto& [ptr, size] : ptrs) 
            {
                MemoryPool::deallocate(ptr, size);
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // 测试new/delete
        {
            Timer t;
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                size_t size = SIZES[rand() % 8];
                void* p = new char[size];
                ptrs.emplace_back(p, size);
                
                if (i % 100 == 0 && !ptrs.empty()) 
                {
                    size_t releaseCount = std::min(ptrs.size(), size_t(20));
                    for (size_t j = 0; j < releaseCount; ++j) 
                    {
                        delete[] static_cast<char*>(ptrs.back().first);
                        ptrs.pop_back();
                    }
                }
            }
            
            for (const auto& [ptr, size] : ptrs) 
            {
                delete[] static_cast<char*>(ptr);
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }

    // 5. V1 对比测试 (完全复刻 V1 的测试逻辑)
    static void testV1Comparable()
    {
        // P1: int (4 bytes) -> 8
        // P2: int[5] (20 bytes) -> 24
        // P3: int[10] (40 bytes) -> 40
        // P4: int[20] (80 bytes) -> 80
        const size_t SIZES[] = {4, 20, 40, 80};
        const size_t NTIMES = 100000;
        const size_t ROUNDS = 10;
        
        std::cout << "\n=== V1 Comparable Benchmark ===" << std::endl;

        auto runBenchmark = [&](size_t nworks)
        {
            std::cout << "Threads: " << nworks << ", Rounds: " << ROUNDS 
                      << ", Iterations: " << NTIMES << std::endl;

            // Memory Pool Test
            {
                Timer t1;
                std::vector<std::thread> threads;
                for(size_t k=0; k<nworks; ++k)
                {
                    threads.emplace_back([&]()
                    {
                        for(size_t j=0; j<ROUNDS; ++j)
                        {
                            for(size_t i=0; i<NTIMES; ++i)
                            {
                                void* p1 = MemoryPool::allocate(SIZES[0]);
                                MemoryPool::deallocate(p1, SIZES[0]);
                                void* p2 = MemoryPool::allocate(SIZES[1]);
                                MemoryPool::deallocate(p2, SIZES[1]);
                                void* p3 = MemoryPool::allocate(SIZES[2]);
                                MemoryPool::deallocate(p3, SIZES[2]);
                                void* p4 = MemoryPool::allocate(SIZES[3]);
                                MemoryPool::deallocate(p4, SIZES[3]);
                            }
                        }
                    });
                }
                for(auto& t : threads) t.join();
                std::cout << "Memory Pool: " << t1.elapsed() << " ms" << std::endl;
            }

            // System New/Delete Test
            {
                Timer t2;
                std::vector<std::thread> threads;
                for(size_t k=0; k<nworks; ++k)
                {
                    threads.emplace_back([&]()
                    {
                        for(size_t j=0; j<ROUNDS; ++j)
                        {
                            for(size_t i=0; i<NTIMES; ++i)
                            {
                                void* p1 = new char[SIZES[0]];
                                delete[] (char*)p1;
                                void* p2 = new char[SIZES[1]];
                                delete[] (char*)p2;
                                void* p3 = new char[SIZES[2]];
                                delete[] (char*)p3;
                                void* p4 = new char[SIZES[3]];
                                delete[] (char*)p4;
                            }
                        }
                    });
                }
                for(auto& t : threads) t.join();
                std::cout << "New/Delete:  " << t2.elapsed() << " ms" << std::endl;
            }
        };

        std::cout << "--- Single Thread ---" << std::endl;
        runBenchmark(1);

        std::cout << "--- Multi Thread (4 threads) ---" << std::endl;
        runBenchmark(4);
    }
};

int main() 
{
    std::cout << "Starting performance tests..." << std::endl;
    
    // 预热系统
    PerformanceTest::warmup();
    
    // 运行测试
    PerformanceTest::testSmallAllocation();
    PerformanceTest::testMultiThreaded();
    PerformanceTest::testMixedSizes();
    PerformanceTest::testV1Comparable();
    
    return 0;
}