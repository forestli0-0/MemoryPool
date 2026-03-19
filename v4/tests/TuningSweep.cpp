#include "../include/MemoryPool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
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

struct ScenarioMetrics
{
    double workloadMs = 0.0;
    double scavengeMs = 0.0;
    double centralAcquire = 0.0;
    double centralRelease = 0.0;
    double pageCacheHit = 0.0;
    double pageCacheMiss = 0.0;
    double systemAlloc = 0.0;
    double systemFree = 0.0;
};

struct SweepPreset
{
    std::string name;
    MemoryPoolRuntimeTuning tuning;
};

struct SweepResult
{
    SweepPreset preset;
    ScenarioMetrics steadyState;
    ScenarioMetrics crossThread;
    ScenarioMetrics mixedRealistic;
    double score = 0.0;
};

struct SweepOptions
{
    size_t runs = 3;
    std::string mode = "presets";
    std::string csvPath;
    size_t top = 10;
    bool compact = false;
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

template <typename Fn>
ScenarioMetrics runV4Scenario(Fn&& fn)
{
    MemoryPool::scavenge();
    const MemoryPoolStats before = MemoryPool::getStats();

    Timer timer;
    const std::uintptr_t localSink = fn();
    const double workloadMs = timer.elapsedMilliseconds();
    const MemoryPoolStats afterWorkload = MemoryPool::getStats();

    Timer scavengeTimer;
    MemoryPool::scavenge();
    const double scavengeMs = scavengeTimer.elapsedMilliseconds();

    commitBenchmarkSink(localSink);

    ScenarioMetrics metrics;
    metrics.workloadMs = workloadMs;
    metrics.scavengeMs = scavengeMs;
    metrics.centralAcquire = static_cast<double>(afterWorkload.central_cache.acquire_calls - before.central_cache.acquire_calls);
    metrics.centralRelease = static_cast<double>(afterWorkload.central_cache.release_calls - before.central_cache.release_calls);
    metrics.pageCacheHit = static_cast<double>(afterWorkload.page_allocator.cache_hit_allocs - before.page_allocator.cache_hit_allocs);
    metrics.pageCacheMiss = static_cast<double>(afterWorkload.page_allocator.cache_miss_allocs - before.page_allocator.cache_miss_allocs);
    metrics.systemAlloc = static_cast<double>(afterWorkload.page_allocator.system_alloc_calls - before.page_allocator.system_alloc_calls);
    metrics.systemFree = static_cast<double>(afterWorkload.page_allocator.system_free_calls - before.page_allocator.system_free_calls);
    return metrics;
}

template <typename Fn>
double runNewDeleteScenario(Fn&& fn)
{
    Timer timer;
    commitBenchmarkSink(fn());
    return timer.elapsedMilliseconds();
}

template <typename Fn>
ScenarioMetrics averageV4Scenario(size_t runs, Fn&& fn)
{
    ScenarioMetrics average;
    for (size_t run = 0; run < runs; ++run)
    {
        const ScenarioMetrics result = runV4Scenario(fn);
        average.workloadMs += result.workloadMs;
        average.scavengeMs += result.scavengeMs;
        average.centralAcquire += result.centralAcquire;
        average.centralRelease += result.centralRelease;
        average.pageCacheHit += result.pageCacheHit;
        average.pageCacheMiss += result.pageCacheMiss;
        average.systemAlloc += result.systemAlloc;
        average.systemFree += result.systemFree;
    }

    const double divisor = static_cast<double>(runs);
    average.workloadMs /= divisor;
    average.scavengeMs /= divisor;
    average.centralAcquire /= divisor;
    average.centralRelease /= divisor;
    average.pageCacheHit /= divisor;
    average.pageCacheMiss /= divisor;
    average.systemAlloc /= divisor;
    average.systemFree /= divisor;
    return average;
}

template <typename Fn>
double averageNewDeleteScenario(size_t runs, Fn&& fn)
{
    double totalMs = 0.0;
    for (size_t run = 0; run < runs; ++run)
    {
        totalMs += runNewDeleteScenario(fn);
    }

    return totalMs / static_cast<double>(runs);
}

template <typename Worker>
double runSteadyStateMultiThreadBenchmark(size_t threadCount, Worker&& worker, std::uintptr_t& totalSink)
{
    std::vector<std::uintptr_t> threadSinks(threadCount, 0);
    std::atomic<size_t> readyCount{0};
    std::atomic<size_t> doneCount{0};
    std::atomic<bool> startFlag{false};
    std::atomic<bool> exitFlag{false};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &worker, &threadSinks, &readyCount, &doneCount, &startFlag, &exitFlag]()
        {
            readyCount.fetch_add(1);
            while (!startFlag.load())
            {
                std::this_thread::yield();
            }

            threadSinks[threadIndex] = worker(threadIndex);
            doneCount.fetch_add(1);

            while (!exitFlag.load())
            {
                std::this_thread::yield();
            }
        });
    }

    while (readyCount.load() != threadCount)
    {
        std::this_thread::yield();
    }

    Timer timer;
    startFlag.store(true);

    while (doneCount.load() != threadCount)
    {
        std::this_thread::yield();
    }

    const double elapsed = timer.elapsedMilliseconds();
    exitFlag.store(true);

    for (auto& thread : threads)
    {
        thread.join();
    }

    totalSink = 0;
    for (std::uintptr_t sink : threadSinks)
    {
        totalSink += sink;
    }

    return elapsed;
}

std::uintptr_t runSteadyStateMemoryPool()
{
    constexpr size_t threadCount = 4;
    constexpr size_t iterations = 100000;
    constexpr std::array<size_t, 4> sizes = {24, 96, 384, 1024};

    std::uintptr_t totalSink = 0;
    runSteadyStateMultiThreadBenchmark(
        threadCount,
        [&sizes](size_t threadIndex)
        {
            std::uintptr_t localSink = 0;
            for (size_t index = 0; index < iterations; ++index)
            {
                const size_t size = sizes[(index + threadIndex) % sizes.size()];
                void* ptr = MemoryPool::allocate(size);
                localSink += touchAllocation(ptr, size);
                MemoryPool::deallocate(ptr, size);
            }
            return localSink;
        },
        totalSink);
    return totalSink;
}

std::uintptr_t runSteadyStateNewDelete()
{
    constexpr size_t threadCount = 4;
    constexpr size_t iterations = 100000;
    constexpr std::array<size_t, 4> sizes = {24, 96, 384, 1024};

    std::uintptr_t totalSink = 0;
    runSteadyStateMultiThreadBenchmark(
        threadCount,
        [&sizes](size_t threadIndex)
        {
            std::uintptr_t localSink = 0;
            for (size_t index = 0; index < iterations; ++index)
            {
                const size_t size = sizes[(index + threadIndex) % sizes.size()];
                char* ptr = new char[size];
                localSink += touchAllocation(ptr, size);
                delete[] ptr;
            }
            return localSink;
        },
        totalSink);
    return totalSink;
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

std::vector<SweepPreset> makePresets()
{
    const MemoryPoolRuntimeTuning defaults = MemoryPoolTuning::defaults();

    MemoryPoolRuntimeTuning batch12k = defaults;
    batch12k.batch_target_bytes = 12 * 1024;
    batch12k.batch_max_count = 96;

    MemoryPoolRuntimeTuning thread48 = defaults;
    thread48.thread_cache_retain_batches = 4;
    thread48.thread_cache_high_water_batches = 8;

    MemoryPoolRuntimeTuning reservoir842 = defaults;
    reservoir842.central_empty_pools_small = 8;
    reservoir842.central_empty_pools_medium = 4;
    reservoir842.central_empty_pools_large = 2;

    MemoryPoolRuntimeTuning lazyPage = defaults;
    lazyPage.page_release_high_water_pages = 2048;
    lazyPage.page_release_low_water_pages = 1024;
    lazyPage.page_auto_scavenge_trigger_pages = 8192;

    MemoryPoolRuntimeTuning batchThread = batch12k;
    batchThread.thread_cache_retain_batches = 4;
    batchThread.thread_cache_high_water_batches = 8;

    MemoryPoolRuntimeTuning batchThreadReservoir = batchThread;
    batchThreadReservoir.central_empty_pools_small = 8;
    batchThreadReservoir.central_empty_pools_medium = 4;
    batchThreadReservoir.central_empty_pools_large = 2;

    MemoryPoolRuntimeTuning allLazy = batchThreadReservoir;
    allLazy.page_release_high_water_pages = 2048;
    allLazy.page_release_low_water_pages = 1024;
    allLazy.page_auto_scavenge_trigger_pages = 8192;

    return {
        {"default", defaults},
        {"batch_12k", batch12k},
        {"thread_4x8", thread48},
        {"empty_842", reservoir842},
        {"lazy_page", lazyPage},
        {"batch_thread", batchThread},
        {"batch_thread_empty", batchThreadReservoir},
        {"all_lazy", allLazy},
    };
}

std::vector<SweepPreset> makeGridLikePresets(
    const std::vector<size_t>& batchTargetBytes,
    const std::vector<size_t>& batchMaxCounts,
    const std::vector<std::pair<size_t, size_t>>& threadWatermarks,
    const std::vector<std::array<size_t, 3>>& emptyPoolReservoirs,
    const std::vector<std::array<size_t, 3>>& pageThresholds,
    const std::string& prefix)
{
    std::vector<SweepPreset> presets;
    presets.reserve(
        batchTargetBytes.size() *
        batchMaxCounts.size() *
        threadWatermarks.size() *
        emptyPoolReservoirs.size() *
        pageThresholds.size());

    for (size_t batchBytes : batchTargetBytes)
    {
        for (size_t batchMax : batchMaxCounts)
        {
            for (const auto& [retainBatches, highWaterBatches] : threadWatermarks)
            {
                for (const auto& emptyPools : emptyPoolReservoirs)
                {
                    for (const auto& pageConfig : pageThresholds)
                    {
                        MemoryPoolRuntimeTuning tuning = MemoryPoolTuning::defaults();
                        tuning.batch_target_bytes = batchBytes;
                        tuning.batch_max_count = batchMax;
                        tuning.thread_cache_retain_batches = retainBatches;
                        tuning.thread_cache_high_water_batches = highWaterBatches;
                        tuning.central_empty_pools_small = emptyPools[0];
                        tuning.central_empty_pools_medium = emptyPools[1];
                        tuning.central_empty_pools_large = emptyPools[2];
                        tuning.page_release_high_water_pages = pageConfig[0];
                        tuning.page_release_low_water_pages = pageConfig[1];
                        tuning.page_auto_scavenge_trigger_pages = pageConfig[2];

                        std::ostringstream name;
                        name << prefix
                             << "_b" << (batchBytes / 1024) << "k"
                             << "_m" << batchMax
                             << "_t" << retainBatches << "x" << highWaterBatches
                             << "_e" << emptyPools[0] << emptyPools[1] << emptyPools[2]
                             << "_p" << pageConfig[0] << "_" << pageConfig[1] << "_" << pageConfig[2];
                        presets.push_back({name.str(), tuning});
                    }
                }
            }
        }
    }

    return presets;
}

std::vector<SweepPreset> makeGridPresets()
{
    return makeGridLikePresets(
        {8 * 1024, 12 * 1024, 16 * 1024},
        {64, 96},
        {{2, 4}, {4, 8}},
        {{4, 2, 1}, {8, 4, 2}},
        {{1024, 512, 4096}, {2048, 1024, 8192}},
        "g");
}

std::vector<SweepPreset> makeFrontierPresets()
{
    return makeGridLikePresets(
        {16 * 1024, 20 * 1024, 24 * 1024},
        {96, 128},
        {{4, 8}, {6, 12}},
        {{8, 4, 2}, {12, 6, 3}},
        {{1024, 512, 4096}, {2048, 1024, 8192}},
        "f");
}

std::vector<SweepPreset> makePresetsForMode(const std::string& mode)
{
    if (mode == "grid")
    {
        return makeGridPresets();
    }

    if (mode == "frontier")
    {
        return makeFrontierPresets();
    }

    return makePresets();
}

void printConfig(const MemoryPoolRuntimeTuning& tuning)
{
    std::cout << "    batch=" << tuning.batch_target_bytes
              << "/" << tuning.batch_max_count
              << " thread=" << tuning.thread_cache_retain_batches
              << "/" << tuning.thread_cache_high_water_batches
              << " empty=" << tuning.central_empty_pools_small
              << "/" << tuning.central_empty_pools_medium
              << "/" << tuning.central_empty_pools_large
              << " page=" << tuning.page_release_high_water_pages
              << "/" << tuning.page_release_low_water_pages
              << "/" << tuning.page_auto_scavenge_trigger_pages
              << std::endl;
}

void printUsage()
{
    std::cout << "Usage: tuning_sweep [runs] [--runs N] [--mode presets|grid|frontier] [--csv PATH] [--top N] [--compact]" << std::endl;
}

bool isNumber(const std::string& value)
{
    if (value.empty())
    {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](unsigned char ch)
    {
        return ch >= '0' && ch <= '9';
    });
}

size_t parsePositive(const std::string& value, size_t fallback)
{
    if (!isNumber(value))
    {
        return fallback;
    }

    const unsigned long long parsed = std::strtoull(value.c_str(), nullptr, 10);
    return (parsed > 0) ? static_cast<size_t>(parsed) : fallback;
}

SweepOptions parseOptions(int argc, char* argv[])
{
    SweepOptions options;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--runs" && index + 1 < argc)
        {
            options.runs = parsePositive(argv[++index], options.runs);
            continue;
        }

        if (argument == "--mode" && index + 1 < argc)
        {
            options.mode = argv[++index];
            continue;
        }

        if (argument == "--csv" && index + 1 < argc)
        {
            options.csvPath = argv[++index];
            continue;
        }

        if (argument == "--top" && index + 1 < argc)
        {
            options.top = parsePositive(argv[++index], options.top);
            continue;
        }

        if (argument == "--compact")
        {
            options.compact = true;
            continue;
        }

        if (argument == "--help" || argument == "-h")
        {
            printUsage();
            std::exit(0);
        }

        if (isNumber(argument))
        {
            options.runs = parsePositive(argument, options.runs);
            continue;
        }

        std::cerr << "Warning: ignoring unknown argument: " << argument << std::endl;
    }

    if (options.mode != "presets" && options.mode != "grid" && options.mode != "frontier")
    {
        std::cerr << "Warning: unsupported mode '" << options.mode << "', falling back to presets." << std::endl;
        options.mode = "presets";
    }

    return options;
}

void writeCsv(const std::string& path,
              const std::vector<SweepResult>& results,
              double steadyNewDeleteMs,
              double crossNewDeleteMs,
              double mixedNewDeleteMs)
{
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output)
    {
        std::cerr << "Warning: failed to open CSV path: " << path << std::endl;
        return;
    }

    output << "name"
           << ",rank"
           << ",score"
           << ",batch_target_bytes"
           << ",batch_max_count"
           << ",thread_cache_retain_batches"
           << ",thread_cache_high_water_batches"
           << ",central_empty_pools_small"
           << ",central_empty_pools_medium"
           << ",central_empty_pools_large"
           << ",page_release_high_water_pages"
           << ",page_release_low_water_pages"
           << ",page_auto_scavenge_trigger_pages"
           << ",steady_new_delete_ms"
           << ",steady_workload_ms"
           << ",steady_scavenge_ms"
           << ",steady_total_ms"
           << ",steady_ratio_vs_new_delete"
           << ",steady_central_acquire"
           << ",steady_central_release"
           << ",cross_new_delete_ms"
           << ",cross_workload_ms"
           << ",cross_scavenge_ms"
           << ",cross_total_ms"
           << ",cross_ratio_vs_new_delete"
           << ",cross_central_acquire"
           << ",cross_central_release"
           << ",mixed_new_delete_ms"
           << ",mixed_workload_ms"
           << ",mixed_scavenge_ms"
           << ",mixed_total_ms"
           << ",mixed_ratio_vs_new_delete"
           << ",mixed_central_acquire"
           << ",mixed_central_release"
           << ",mixed_page_cache_hit"
           << ",mixed_page_cache_miss"
           << ",mixed_system_alloc"
           << ",mixed_system_free"
           << '\n';

    for (size_t index = 0; index < results.size(); ++index)
    {
        const SweepResult& result = results[index];
        const auto& tuning = result.preset.tuning;
        output << result.preset.name
               << ',' << (index + 1)
               << ',' << result.score
               << ',' << tuning.batch_target_bytes
               << ',' << tuning.batch_max_count
               << ',' << tuning.thread_cache_retain_batches
               << ',' << tuning.thread_cache_high_water_batches
               << ',' << tuning.central_empty_pools_small
               << ',' << tuning.central_empty_pools_medium
               << ',' << tuning.central_empty_pools_large
               << ',' << tuning.page_release_high_water_pages
               << ',' << tuning.page_release_low_water_pages
               << ',' << tuning.page_auto_scavenge_trigger_pages
               << ',' << steadyNewDeleteMs
               << ',' << result.steadyState.workloadMs
               << ',' << result.steadyState.scavengeMs
               << ',' << (result.steadyState.workloadMs + result.steadyState.scavengeMs)
               << ',' << (result.steadyState.workloadMs / steadyNewDeleteMs)
               << ',' << result.steadyState.centralAcquire
               << ',' << result.steadyState.centralRelease
               << ',' << crossNewDeleteMs
               << ',' << result.crossThread.workloadMs
               << ',' << result.crossThread.scavengeMs
               << ',' << (result.crossThread.workloadMs + result.crossThread.scavengeMs)
               << ',' << (result.crossThread.workloadMs / crossNewDeleteMs)
               << ',' << result.crossThread.centralAcquire
               << ',' << result.crossThread.centralRelease
               << ',' << mixedNewDeleteMs
               << ',' << result.mixedRealistic.workloadMs
               << ',' << result.mixedRealistic.scavengeMs
               << ',' << (result.mixedRealistic.workloadMs + result.mixedRealistic.scavengeMs)
               << ',' << (result.mixedRealistic.workloadMs / mixedNewDeleteMs)
               << ',' << result.mixedRealistic.centralAcquire
               << ',' << result.mixedRealistic.centralRelease
               << ',' << result.mixedRealistic.pageCacheHit
               << ',' << result.mixedRealistic.pageCacheMiss
               << ',' << result.mixedRealistic.systemAlloc
               << ',' << result.mixedRealistic.systemFree
               << '\n';
    }
}

} // namespace

int main(int argc, char* argv[])
{
    const SweepOptions options = parseOptions(argc, argv);
    const size_t runs = options.runs;
    const std::vector<SweepPreset> presets = makePresetsForMode(options.mode);

    std::cout << "Starting V4 tuning sweep..." << std::endl;
    std::cout << "Runs per preset: " << runs << std::endl;
    std::cout << "Mode: " << options.mode << std::endl;
    std::cout << "Presets: " << presets.size() << std::endl;

    MemoryPool::resetTuning();
    warmup();

    const double steadyNewDeleteMs = averageNewDeleteScenario(runs, runSteadyStateNewDelete);
    const double crossNewDeleteMs = averageNewDeleteScenario(runs, runCrossThreadHandoffNewDelete);
    const double mixedNewDeleteMs = averageNewDeleteScenario(runs, runMixedRealisticNewDelete);

    std::cout << "\nBaselines:" << std::endl;
    std::cout << "  Steady State new/delete          " << std::fixed << std::setprecision(3)
              << steadyNewDeleteMs << " ms" << std::endl;
    std::cout << "  Cross Thread Handoff new/delete  " << std::fixed << std::setprecision(3)
              << crossNewDeleteMs << " ms" << std::endl;
    std::cout << "  Mixed Realistic new/delete       " << std::fixed << std::setprecision(3)
              << mixedNewDeleteMs << " ms" << std::endl;

    std::vector<SweepResult> results;
    results.reserve(presets.size());

    for (const SweepPreset& preset : presets)
    {
        MemoryPool::applyTuning(preset.tuning);
        warmup();

        SweepResult result;
        result.preset = preset;
        result.steadyState = averageV4Scenario(runs, runSteadyStateMemoryPool);
        result.crossThread = averageV4Scenario(runs, runCrossThreadHandoffMemoryPool);
        result.mixedRealistic = averageV4Scenario(runs, runMixedRealisticMemoryPool);
        result.score =
            (result.steadyState.workloadMs / steadyNewDeleteMs) +
            (result.crossThread.workloadMs / crossNewDeleteMs) +
            (result.mixedRealistic.workloadMs / mixedNewDeleteMs);
        results.push_back(result);

        if (options.compact)
        {
            std::cout << "  " << std::left << std::setw(28) << preset.name
                      << " score=" << std::fixed << std::setprecision(3) << result.score
                      << " steady=" << result.steadyState.workloadMs
                      << " (" << (result.steadyState.workloadMs / steadyNewDeleteMs) << "x)"
                      << " cross=" << result.crossThread.workloadMs
                      << " (" << (result.crossThread.workloadMs / crossNewDeleteMs) << "x)"
                      << " mixed=" << result.mixedRealistic.workloadMs
                      << " (" << (result.mixedRealistic.workloadMs / mixedNewDeleteMs) << "x)"
                      << " s_a/r=" << result.steadyState.centralAcquire << "/" << result.steadyState.centralRelease
                      << " c_a/r=" << result.crossThread.centralAcquire << "/" << result.crossThread.centralRelease
                      << " m_a/r=" << result.mixedRealistic.centralAcquire << "/" << result.mixedRealistic.centralRelease
                      << std::endl;
        }
        else
        {
            std::cout << "\n[" << preset.name << "]" << std::endl;
            printConfig(preset.tuning);
            std::cout << "    steady workload=" << std::fixed << std::setprecision(3)
                      << result.steadyState.workloadMs << " ms"
                      << " scavenge=" << result.steadyState.scavengeMs << " ms"
                      << " central a/r=" << result.steadyState.centralAcquire
                      << "/" << result.steadyState.centralRelease
                      << std::endl;
            std::cout << "    cross workload=" << std::fixed << std::setprecision(3)
                      << result.crossThread.workloadMs << " ms"
                      << " scavenge=" << result.crossThread.scavengeMs << " ms"
                      << " central a/r=" << result.crossThread.centralAcquire
                      << "/" << result.crossThread.centralRelease
                      << std::endl;
            std::cout << "    mixed workload=" << std::fixed << std::setprecision(3)
                      << result.mixedRealistic.workloadMs << " ms"
                      << " scavenge=" << result.mixedRealistic.scavengeMs << " ms"
                      << " central a/r=" << result.mixedRealistic.centralAcquire
                      << "/" << result.mixedRealistic.centralRelease
                      << " page hit/miss/sysA/sysF="
                      << result.mixedRealistic.pageCacheHit << "/"
                      << result.mixedRealistic.pageCacheMiss << "/"
                      << result.mixedRealistic.systemAlloc << "/"
                      << result.mixedRealistic.systemFree
                      << std::endl;
            std::cout << "    score=" << std::fixed << std::setprecision(3)
                      << result.score << std::endl;
        }
    }

    std::sort(results.begin(), results.end(), [](const SweepResult& lhs, const SweepResult& rhs)
    {
        return lhs.score < rhs.score;
    });

    if (!options.csvPath.empty())
    {
        writeCsv(options.csvPath, results, steadyNewDeleteMs, crossNewDeleteMs, mixedNewDeleteMs);
        std::cout << "\nCSV written to: " << options.csvPath << std::endl;
    }

    std::cout << "\nRanking:" << std::endl;
    const size_t rankingCount = std::min(results.size(), options.top);
    for (size_t index = 0; index < rankingCount; ++index)
    {
        const SweepResult& result = results[index];
        std::cout << "  #" << (index + 1) << " " << std::left << std::setw(18) << result.preset.name
                  << " score=" << std::fixed << std::setprecision(3) << result.score
                  << " steady=" << result.steadyState.workloadMs
                  << " cross=" << result.crossThread.workloadMs
                  << " mixed=" << result.mixedRealistic.workloadMs
                  << std::endl;
    }

    MemoryPool::resetTuning();
    std::cout << "\nBenchmark sink: " << benchmarkSink << std::endl;
    return 0;
}
