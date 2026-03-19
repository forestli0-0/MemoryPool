#include "../include/MemoryPool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace glock;

namespace
{

struct Allocation
{
    void* ptr = nullptr;
    size_t size = 0;
};

struct TimedAllocation
{
    Allocation allocation;
    size_t framesLeft = 0;
};

struct ScenarioStats
{
    MemoryPoolStats before;
    MemoryPoolStats afterWorkload;
    MemoryPoolStats afterScavenge;
};

struct ScenarioResult
{
    double elapsedMs = 0.0;
    double scavengeMs = 0.0;
    ScenarioStats stats;
};

volatile std::uintptr_t benchmarkSink = 0;

class Timer
{
public:
    Timer()
        : start_(std::chrono::steady_clock::now())
    {
    }

    double elapsedMilliseconds() const
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

std::uintptr_t touchAllocation(void* ptr, size_t size)
{
    auto* bytes = static_cast<unsigned char*>(ptr);
    bytes[0] = static_cast<unsigned char>(size & 0xFF);
    bytes[size - 1] = static_cast<unsigned char>((size >> 1) & 0xFF);
    return reinterpret_cast<std::uintptr_t>(ptr) ^ bytes[0] ^ bytes[size - 1];
}

void commitBenchmarkSink(std::uintptr_t value)
{
    benchmarkSink += value;
}

void warmup()
{
    for (size_t index = 0; index < 5000; ++index)
    {
        void* ptr = MemoryPool::allocate(64);
        MemoryPool::deallocate(ptr, 64);
    }
    MemoryPool::scavenge();
}

void printStateLine(const std::string& label, const MemoryPoolStats& stats)
{
    std::cout << "  " << std::left << std::setw(16) << label
              << " active_pools=" << std::setw(4) << stats.active_pool_count
              << " empty_pools=" << std::setw(4) << stats.empty_pool_count
              << " cached_pages=" << std::setw(6) << stats.cached_free_pages
              << " freeable_pages=" << std::setw(6) << stats.page_allocator.immediately_freeable_pages
              << " os_reserved=" << std::setw(10) << stats.os_reserved_bytes
              << " os_released=" << stats.os_released_bytes
              << std::endl;
}

void printRequestDelta(const MemoryPoolStats& before, const MemoryPoolStats& after)
{
    std::cout << "  requests         small+"
              << (after.small_alloc_requests - before.small_alloc_requests)
              << " large+"
              << (after.large_alloc_requests - before.large_alloc_requests)
              << std::endl;
}

void printByteDelta(const MemoryPoolStats& before, const MemoryPoolStats& after)
{
    std::cout << "  bytes            reserved+"
              << (after.os_reserved_bytes - before.os_reserved_bytes)
              << " released+"
              << (after.os_released_bytes - before.os_released_bytes)
              << std::endl;
}

void printThreadCacheDelta(const std::string& label, const MemoryPoolStats& before, const MemoryPoolStats& after)
{
    const auto& beforeStats = before.thread_cache;
    const auto& afterStats = after.thread_cache;
    std::cout << "  " << std::left << std::setw(16) << label
              << " hit+" << (afterStats.alloc_hits - beforeStats.alloc_hits)
              << " miss+" << (afterStats.alloc_misses - beforeStats.alloc_misses)
              << " fetch+" << (afterStats.batch_fetches - beforeStats.batch_fetches)
              << " fetched_blk+" << (afterStats.blocks_fetched - beforeStats.blocks_fetched)
              << " return+" << (afterStats.batch_returns - beforeStats.batch_returns)
              << " returned_blk+" << (afterStats.blocks_returned - beforeStats.blocks_returned)
              << " flushed_blk+" << (afterStats.blocks_flushed - beforeStats.blocks_flushed)
              << " large_a+" << (afterStats.large_alloc_direct - beforeStats.large_alloc_direct)
              << " large_f+" << (afterStats.large_free_direct - beforeStats.large_free_direct)
              << std::endl;
}

void printCentralCacheDelta(const std::string& label, const MemoryPoolStats& before, const MemoryPoolStats& after)
{
    const auto& beforeStats = before.central_cache;
    const auto& afterStats = after.central_cache;
    std::cout << "  " << std::left << std::setw(16) << label
              << " acquire+" << (afterStats.acquire_calls - beforeStats.acquire_calls)
              << " release+" << (afterStats.release_calls - beforeStats.release_calls)
              << " blk_out+" << (afterStats.blocks_acquired - beforeStats.blocks_acquired)
              << " blk_in+" << (afterStats.blocks_released - beforeStats.blocks_released)
              << " partial+" << (afterStats.partial_pool_hits - beforeStats.partial_pool_hits)
              << " empty+" << (afterStats.empty_pool_hits - beforeStats.empty_pool_hits)
              << " create+" << (afterStats.pools_created - beforeStats.pools_created)
              << " release_pool+" << (afterStats.pools_released - beforeStats.pools_released)
              << " scav+" << (afterStats.scavenge_calls - beforeStats.scavenge_calls)
              << std::endl;
}

void printPageAllocatorDelta(const std::string& label, const MemoryPoolStats& before, const MemoryPoolStats& after)
{
    const auto& beforeStats = before.page_allocator;
    const auto& afterStats = after.page_allocator;
    std::cout << "  " << std::left << std::setw(16) << label
              << " span_a+" << (afterStats.span_alloc_calls - beforeStats.span_alloc_calls)
              << " span_f+" << (afterStats.span_free_calls - beforeStats.span_free_calls)
              << " cache_hit+" << (afterStats.cache_hit_allocs - beforeStats.cache_hit_allocs)
              << " cache_miss+" << (afterStats.cache_miss_allocs - beforeStats.cache_miss_allocs)
              << " sys_a+" << (afterStats.system_alloc_calls - beforeStats.system_alloc_calls)
              << " sys_f+" << (afterStats.system_free_calls - beforeStats.system_free_calls)
              << " merge+" << (afterStats.coalesce_merges - beforeStats.coalesce_merges)
              << " released+" << (afterStats.released_spans - beforeStats.released_spans)
              << " scavenge+" << (afterStats.scavenge_calls - beforeStats.scavenge_calls)
              << std::endl;
}

template <typename Fn>
ScenarioResult runV4Scenario(Fn&& fn)
{
    MemoryPool::scavenge();

    ScenarioResult result;
    result.stats.before = MemoryPool::getStats();

    Timer timer;
    const std::uintptr_t localSink = fn();
    result.elapsedMs = timer.elapsedMilliseconds();
    result.stats.afterWorkload = MemoryPool::getStats();

    Timer scavengeTimer;
    MemoryPool::scavenge();
    result.scavengeMs = scavengeTimer.elapsedMilliseconds();
    result.stats.afterScavenge = MemoryPool::getStats();

    commitBenchmarkSink(localSink);
    return result;
}

template <typename Fn>
double runNewDeleteScenario(Fn&& fn)
{
    Timer timer;
    commitBenchmarkSink(fn());
    return timer.elapsedMilliseconds();
}

void printComparison(const std::string& name,
                     const std::string& description,
                     const ScenarioResult& v4Result,
                     double newDeleteMs)
{
    std::cout << "\n=== " << name << " ===" << std::endl;
    std::cout << description << std::endl;
    std::cout << "  V4 workload      " << std::fixed << std::setprecision(3)
              << v4Result.elapsedMs << " ms" << std::endl;
    std::cout << "  V4 scavenge      " << std::fixed << std::setprecision(3)
              << v4Result.scavengeMs << " ms" << std::endl;
    std::cout << "  new/delete       " << std::fixed << std::setprecision(3)
              << newDeleteMs << " ms" << std::endl;

    const double ratio = (newDeleteMs > 0.0) ? (v4Result.elapsedMs / newDeleteMs) : 0.0;
    std::cout << "  ratio            " << std::fixed << std::setprecision(3)
              << ratio << "x of new/delete"
              << ((ratio < 1.0) ? " (V4 faster)" : " (V4 slower)")
              << std::endl;

    printRequestDelta(v4Result.stats.before, v4Result.stats.afterWorkload);
    printByteDelta(v4Result.stats.before, v4Result.stats.afterScavenge);
    printThreadCacheDelta("thread workload", v4Result.stats.before, v4Result.stats.afterWorkload);
    printCentralCacheDelta("central workload", v4Result.stats.before, v4Result.stats.afterWorkload);
    printPageAllocatorDelta("page workload", v4Result.stats.before, v4Result.stats.afterWorkload);
    printThreadCacheDelta("thread scavenge", v4Result.stats.afterWorkload, v4Result.stats.afterScavenge);
    printCentralCacheDelta("central scavenge", v4Result.stats.afterWorkload, v4Result.stats.afterScavenge);
    printPageAllocatorDelta("page scavenge", v4Result.stats.afterWorkload, v4Result.stats.afterScavenge);
    printStateLine("before", v4Result.stats.before);
    printStateLine("after workload", v4Result.stats.afterWorkload);
    printStateLine("after scavenge", v4Result.stats.afterScavenge);
}

std::uintptr_t runFrameSmallChurnMemoryPool()
{
    constexpr std::array<size_t, 6> sizes = {24, 48, 96, 160, 256, 512};
    constexpr size_t frames = 180;

    std::uintptr_t localSink = 0;
    std::vector<Allocation> frameAllocations;
    frameAllocations.reserve(4096);

    for (size_t frame = 0; frame < frames; ++frame)
    {
        frameAllocations.clear();
        const size_t allocationCount = 2048 + (frame % 4) * 512;
        for (size_t index = 0; index < allocationCount; ++index)
        {
            const size_t size = sizes[(frame + index) % sizes.size()];
            void* ptr = MemoryPool::allocate(size);
            localSink += touchAllocation(ptr, size);
            frameAllocations.push_back({ptr, size});
        }

        for (auto it = frameAllocations.rbegin(); it != frameAllocations.rend(); ++it)
        {
            MemoryPool::deallocate(it->ptr, it->size);
        }
    }

    return localSink;
}

std::uintptr_t runFrameSmallChurnNewDelete()
{
    constexpr std::array<size_t, 6> sizes = {24, 48, 96, 160, 256, 512};
    constexpr size_t frames = 180;

    std::uintptr_t localSink = 0;
    std::vector<Allocation> frameAllocations;
    frameAllocations.reserve(4096);

    for (size_t frame = 0; frame < frames; ++frame)
    {
        frameAllocations.clear();
        const size_t allocationCount = 2048 + (frame % 4) * 512;
        for (size_t index = 0; index < allocationCount; ++index)
        {
            const size_t size = sizes[(frame + index) % sizes.size()];
            char* ptr = new char[size];
            localSink += touchAllocation(ptr, size);
            frameAllocations.push_back({ptr, size});
        }

        for (auto it = frameAllocations.rbegin(); it != frameAllocations.rend(); ++it)
        {
            delete[] static_cast<char*>(it->ptr);
        }
    }

    return localSink;
}

std::uintptr_t runBurstReuseMemoryPool()
{
    constexpr std::array<size_t, 4> sizes = {32, 64, 128, 256};
    constexpr size_t burstCount = 30000;
    constexpr size_t steadyFrames = 200;
    constexpr size_t steadyCount = 1024;

    std::uintptr_t localSink = 0;
    std::vector<Allocation> live;
    live.reserve(burstCount);

    for (size_t index = 0; index < burstCount; ++index)
    {
        const size_t size = sizes[index % sizes.size()];
        void* ptr = MemoryPool::allocate(size);
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
    }

    for (auto it = live.rbegin(); it != live.rend(); ++it)
    {
        MemoryPool::deallocate(it->ptr, it->size);
    }

    live.clear();
    live.reserve(steadyCount);

    for (size_t frame = 0; frame < steadyFrames; ++frame)
    {
        live.clear();
        for (size_t index = 0; index < steadyCount; ++index)
        {
            const size_t size = sizes[(frame + index) % sizes.size()];
            void* ptr = MemoryPool::allocate(size);
            localSink += touchAllocation(ptr, size);
            live.push_back({ptr, size});
        }

        for (auto it = live.rbegin(); it != live.rend(); ++it)
        {
            MemoryPool::deallocate(it->ptr, it->size);
        }
    }

    return localSink;
}

std::uintptr_t runBurstReuseNewDelete()
{
    constexpr std::array<size_t, 4> sizes = {32, 64, 128, 256};
    constexpr size_t burstCount = 30000;
    constexpr size_t steadyFrames = 200;
    constexpr size_t steadyCount = 1024;

    std::uintptr_t localSink = 0;
    std::vector<Allocation> live;
    live.reserve(burstCount);

    for (size_t index = 0; index < burstCount; ++index)
    {
        const size_t size = sizes[index % sizes.size()];
        char* ptr = new char[size];
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
    }

    for (auto it = live.rbegin(); it != live.rend(); ++it)
    {
        delete[] static_cast<char*>(it->ptr);
    }

    live.clear();
    live.reserve(steadyCount);

    for (size_t frame = 0; frame < steadyFrames; ++frame)
    {
        live.clear();
        for (size_t index = 0; index < steadyCount; ++index)
        {
            const size_t size = sizes[(frame + index) % sizes.size()];
            char* ptr = new char[size];
            localSink += touchAllocation(ptr, size);
            live.push_back({ptr, size});
        }

        for (auto it = live.rbegin(); it != live.rend(); ++it)
        {
            delete[] static_cast<char*>(it->ptr);
        }
    }

    return localSink;
}

std::uintptr_t runCrossThreadHandoffMemoryPool()
{
    constexpr std::array<size_t, 5> sizes = {32, 64, 128, 256, 512};
    constexpr size_t threadCount = 4;
    constexpr size_t allocationsPerThread = 25000;

    std::uintptr_t localSink = 0;
    std::vector<std::vector<Allocation>> batches(threadCount);
    std::vector<std::uintptr_t> producerSinks(threadCount, 0);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &batches, &producerSinks, &sizes]()
        {
            batches[threadIndex].reserve(allocationsPerThread);
            std::uintptr_t threadSink = 0;
            for (size_t index = 0; index < allocationsPerThread; ++index)
            {
                const size_t size = sizes[(threadIndex + index) % sizes.size()];
                void* ptr = MemoryPool::allocate(size);
                threadSink += touchAllocation(ptr, size);
                batches[threadIndex].push_back({ptr, size});
            }
            producerSinks[threadIndex] = threadSink;
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    threads.clear();
    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &batches]()
        {
            const size_t batchIndex = (threadIndex + 1) % threadCount;
            for (const Allocation& allocation : batches[batchIndex])
            {
                MemoryPool::deallocate(allocation.ptr, allocation.size);
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    for (std::uintptr_t threadSink : producerSinks)
    {
        localSink += threadSink;
    }
    return localSink;
}

std::uintptr_t runCrossThreadHandoffNewDelete()
{
    constexpr std::array<size_t, 5> sizes = {32, 64, 128, 256, 512};
    constexpr size_t threadCount = 4;
    constexpr size_t allocationsPerThread = 25000;

    std::uintptr_t localSink = 0;
    std::vector<std::vector<Allocation>> batches(threadCount);
    std::vector<std::uintptr_t> producerSinks(threadCount, 0);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &batches, &producerSinks, &sizes]()
        {
            batches[threadIndex].reserve(allocationsPerThread);
            std::uintptr_t threadSink = 0;
            for (size_t index = 0; index < allocationsPerThread; ++index)
            {
                const size_t size = sizes[(threadIndex + index) % sizes.size()];
                char* ptr = new char[size];
                threadSink += touchAllocation(ptr, size);
                batches[threadIndex].push_back({ptr, size});
            }
            producerSinks[threadIndex] = threadSink;
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    threads.clear();
    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &batches]()
        {
            const size_t batchIndex = (threadIndex + 1) % threadCount;
            for (const Allocation& allocation : batches[batchIndex])
            {
                delete[] static_cast<char*>(allocation.ptr);
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    for (std::uintptr_t threadSink : producerSinks)
    {
        localSink += threadSink;
    }
    return localSink;
}

std::uintptr_t runSceneSwitchMemoryPool()
{
    constexpr std::array<size_t, 6> smallSizes = {48, 64, 96, 160, 256, 512};
    constexpr std::array<size_t, 5> largeSizes = {2048, 4096, 16384, 32768, 65536};
    constexpr size_t smallCount = 40000;
    constexpr size_t largeCount = 512;

    std::uintptr_t localSink = 0;
    std::vector<Allocation> live;
    live.reserve(smallCount + largeCount);

    for (size_t index = 0; index < smallCount; ++index)
    {
        const size_t size = smallSizes[index % smallSizes.size()];
        void* ptr = MemoryPool::allocate(size);
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
    }

    for (size_t index = 0; index < largeCount; ++index)
    {
        const size_t size = largeSizes[index % largeSizes.size()];
        void* ptr = MemoryPool::allocate(size);
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
    }

    for (auto it = live.rbegin(); it != live.rend(); ++it)
    {
        MemoryPool::deallocate(it->ptr, it->size);
    }

    return localSink;
}

std::uintptr_t runSceneSwitchNewDelete()
{
    constexpr std::array<size_t, 6> smallSizes = {48, 64, 96, 160, 256, 512};
    constexpr std::array<size_t, 5> largeSizes = {2048, 4096, 16384, 32768, 65536};
    constexpr size_t smallCount = 40000;
    constexpr size_t largeCount = 512;

    std::uintptr_t localSink = 0;
    std::vector<Allocation> live;
    live.reserve(smallCount + largeCount);

    for (size_t index = 0; index < smallCount; ++index)
    {
        const size_t size = smallSizes[index % smallSizes.size()];
        char* ptr = new char[size];
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
    }

    for (size_t index = 0; index < largeCount; ++index)
    {
        const size_t size = largeSizes[index % largeSizes.size()];
        char* ptr = new char[size];
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
    }

    for (auto it = live.rbegin(); it != live.rend(); ++it)
    {
        delete[] static_cast<char*>(it->ptr);
    }

    return localSink;
}

std::uintptr_t runMixedRealisticMemoryPool()
{
    constexpr std::array<size_t, 7> smallSizes = {24, 40, 64, 96, 160, 256, 512};
    constexpr std::array<size_t, 5> largeSizes = {1280, 4096, 12288, 32768, 65536};
    constexpr size_t frames = 180;

    std::mt19937 generator(20260313);
    std::uniform_int_distribution<int> smallOrLarge(0, 99);
    std::uniform_int_distribution<size_t> smallPick(0, smallSizes.size() - 1);
    std::uniform_int_distribution<size_t> largePick(0, largeSizes.size() - 1);
    std::uniform_int_distribution<size_t> lifetimePick(1, 4);

    std::uintptr_t localSink = 0;
    std::vector<TimedAllocation> live;
    live.reserve(8192);

    for (size_t frame = 0; frame < frames; ++frame)
    {
        for (size_t index = 0; index < live.size();)
        {
            if (live[index].framesLeft == 0)
            {
                MemoryPool::deallocate(live[index].allocation.ptr, live[index].allocation.size);
                live[index] = live.back();
                live.pop_back();
                continue;
            }

            --live[index].framesLeft;
            ++index;
        }

        const size_t spawnCount = 1200 + (frame % 5) * 150;
        for (size_t index = 0; index < spawnCount; ++index)
        {
            const bool useLarge = smallOrLarge(generator) < 10;
            const size_t size = useLarge ? largeSizes[largePick(generator)] : smallSizes[smallPick(generator)];
            void* ptr = MemoryPool::allocate(size);
            localSink += touchAllocation(ptr, size);
            live.push_back({{ptr, size}, lifetimePick(generator)});
        }
    }

    for (const TimedAllocation& allocation : live)
    {
        MemoryPool::deallocate(allocation.allocation.ptr, allocation.allocation.size);
    }

    return localSink;
}

std::uintptr_t runMixedRealisticNewDelete()
{
    constexpr std::array<size_t, 7> smallSizes = {24, 40, 64, 96, 160, 256, 512};
    constexpr std::array<size_t, 5> largeSizes = {1280, 4096, 12288, 32768, 65536};
    constexpr size_t frames = 180;

    std::mt19937 generator(20260313);
    std::uniform_int_distribution<int> smallOrLarge(0, 99);
    std::uniform_int_distribution<size_t> smallPick(0, smallSizes.size() - 1);
    std::uniform_int_distribution<size_t> largePick(0, largeSizes.size() - 1);
    std::uniform_int_distribution<size_t> lifetimePick(1, 4);

    std::uintptr_t localSink = 0;
    std::vector<TimedAllocation> live;
    live.reserve(8192);

    for (size_t frame = 0; frame < frames; ++frame)
    {
        for (size_t index = 0; index < live.size();)
        {
            if (live[index].framesLeft == 0)
            {
                delete[] static_cast<char*>(live[index].allocation.ptr);
                live[index] = live.back();
                live.pop_back();
                continue;
            }

            --live[index].framesLeft;
            ++index;
        }

        const size_t spawnCount = 1200 + (frame % 5) * 150;
        for (size_t index = 0; index < spawnCount; ++index)
        {
            const bool useLarge = smallOrLarge(generator) < 10;
            const size_t size = useLarge ? largeSizes[largePick(generator)] : smallSizes[smallPick(generator)];
            char* ptr = new char[size];
            localSink += touchAllocation(ptr, size);
            live.push_back({{ptr, size}, lifetimePick(generator)});
        }
    }

    for (const TimedAllocation& allocation : live)
    {
        delete[] static_cast<char*>(allocation.allocation.ptr);
    }

    return localSink;
}

} // namespace

int main()
{
    std::cout << "Starting V4 scenario benchmarks..." << std::endl;
    warmup();

    printComparison(
        "Frame Small Churn",
        "Many small temporary objects are created and destroyed within each frame.",
        runV4Scenario(runFrameSmallChurnMemoryPool),
        runNewDeleteScenario(runFrameSmallChurnNewDelete));

    printComparison(
        "Burst And Reuse",
        "A peak burst fills caches first, then later frames reuse the same object sizes.",
        runV4Scenario(runBurstReuseMemoryPool),
        runNewDeleteScenario(runBurstReuseNewDelete));

    printComparison(
        "Cross Thread Handoff",
        "One wave of threads allocates, another wave releases those objects on different threads.",
        runV4Scenario(runCrossThreadHandoffMemoryPool),
        runNewDeleteScenario(runCrossThreadHandoffNewDelete));

    printComparison(
        "Scene Switch And Scavenge",
        "A scene loads to a high-water mark, frees everything, then asks the allocator to return pages.",
        runV4Scenario(runSceneSwitchMemoryPool),
        runNewDeleteScenario(runSceneSwitchNewDelete));

    printComparison(
        "Mixed Realistic",
        "About 90% small objects and 10% larger allocations with short multi-frame lifetimes.",
        runV4Scenario(runMixedRealisticMemoryPool),
        runNewDeleteScenario(runMixedRealisticNewDelete));

    std::cout << "\nBenchmark sink: " << benchmarkSink << std::endl;
    return 0;
}
