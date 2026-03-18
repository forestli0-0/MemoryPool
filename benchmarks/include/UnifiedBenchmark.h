#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace unified_bench
{

struct ScenarioResult
{
    std::string name;
    double allocator_ms = 0.0;
    double system_ms = 0.0;
};

inline volatile std::uintptr_t benchmarkSink = 0;

inline std::uintptr_t touchAllocation(void* ptr, size_t size)
{
    auto* bytes = static_cast<unsigned char*>(ptr);
    bytes[0] = static_cast<unsigned char>(size & 0xFF);
    bytes[size - 1] = static_cast<unsigned char>((size >> 1) & 0xFF);
    return reinterpret_cast<std::uintptr_t>(ptr) ^ bytes[0] ^ bytes[size - 1];
}

inline void commitSink(std::uintptr_t value)
{
    benchmarkSink += value;
}

class Timer
{
public:
    Timer()
        : start_(std::chrono::steady_clock::now())
    {
    }

    double elapsedMilliseconds() const
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

inline double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

inline std::vector<size_t> makeSmallSizes(size_t count, std::uint32_t seed)
{
    static constexpr size_t kSmallSizes[] = {8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};
    constexpr size_t kSmallSizeCount = sizeof(kSmallSizes) / sizeof(kSmallSizes[0]);

    std::mt19937 generator(seed);
    std::uniform_int_distribution<size_t> distribution(0, kSmallSizeCount - 1);

    std::vector<size_t> sizes;
    sizes.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        sizes.push_back(kSmallSizes[distribution(generator)]);
    }
    return sizes;
}

inline void warmupSystem()
{
    const auto sizes = makeSmallSizes(2000, 777);
    std::uintptr_t localSink = 0;
    for (size_t size : sizes)
    {
        char* ptr = new char[size];
        localSink += touchAllocation(ptr, size);
        delete[] ptr;
    }
    commitSink(localSink);
}

template <typename Allocator>
void warmupAllocator()
{
    const auto sizes = makeSmallSizes(2000, 778);
    std::uintptr_t localSink = 0;
    for (size_t size : sizes)
    {
        void* ptr = Allocator::allocate(size);
        localSink += touchAllocation(ptr, size);
        Allocator::deallocate(ptr, size);
    }
    commitSink(localSink);
}

template <typename Fn>
double runRepeated(Fn&& fn, size_t repeats = 3)
{
    std::vector<double> samples;
    samples.reserve(repeats);
    for (size_t repeat = 0; repeat < repeats; ++repeat)
    {
        samples.push_back(fn());
    }
    return median(samples);
}

template <typename Allocator>
double benchmarkSingleThreadAllocator()
{
    constexpr size_t iterations = 100000;
    constexpr size_t sizes[] = {16, 64, 256, 512};

    std::uintptr_t localSink = 0;
    Timer timer;
    for (size_t index = 0; index < iterations; ++index)
    {
        const size_t size = sizes[index % 4];
        void* ptr = Allocator::allocate(size);
        localSink += touchAllocation(ptr, size);
        Allocator::deallocate(ptr, size);
    }
    const double elapsed = timer.elapsedMilliseconds();
    commitSink(localSink);
    return elapsed;
}

inline double benchmarkSingleThreadSystem()
{
    constexpr size_t iterations = 100000;
    constexpr size_t sizes[] = {16, 64, 256, 512};

    std::uintptr_t localSink = 0;
    Timer timer;
    for (size_t index = 0; index < iterations; ++index)
    {
        const size_t size = sizes[index % 4];
        char* ptr = new char[size];
        localSink += touchAllocation(ptr, size);
        delete[] ptr;
    }
    const double elapsed = timer.elapsedMilliseconds();
    commitSink(localSink);
    return elapsed;
}

template <typename Allocator>
double benchmarkMultiThreadAllocator()
{
    constexpr size_t threadCount = 4;
    constexpr size_t iterationsPerThread = 40000;
    constexpr size_t sizes[] = {24, 96, 384, 512};

    std::vector<std::uintptr_t> sinks(threadCount, 0);
    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &sinks, &sizes]()
        {
            std::uintptr_t localSink = 0;
            for (size_t index = 0; index < iterationsPerThread; ++index)
            {
                const size_t size = sizes[(index + threadIndex) % 4];
                void* ptr = Allocator::allocate(size);
                localSink += touchAllocation(ptr, size);
                Allocator::deallocate(ptr, size);
            }
            sinks[threadIndex] = localSink;
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    const double elapsed = timer.elapsedMilliseconds();
    std::uintptr_t totalSink = 0;
    for (std::uintptr_t sink : sinks)
    {
        totalSink += sink;
    }
    commitSink(totalSink);
    return elapsed;
}

inline double benchmarkMultiThreadSystem()
{
    constexpr size_t threadCount = 4;
    constexpr size_t iterationsPerThread = 40000;
    constexpr size_t sizes[] = {24, 96, 384, 512};

    std::vector<std::uintptr_t> sinks(threadCount, 0);
    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &sinks, &sizes]()
        {
            std::uintptr_t localSink = 0;
            for (size_t index = 0; index < iterationsPerThread; ++index)
            {
                const size_t size = sizes[(index + threadIndex) % 4];
                char* ptr = new char[size];
                localSink += touchAllocation(ptr, size);
                delete[] ptr;
            }
            sinks[threadIndex] = localSink;
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    const double elapsed = timer.elapsedMilliseconds();
    std::uintptr_t totalSink = 0;
    for (std::uintptr_t sink : sinks)
    {
        totalSink += sink;
    }
    commitSink(totalSink);
    return elapsed;
}

template <typename Allocator>
double benchmarkMixedSmallChurnAllocator()
{
    const auto sizes = makeSmallSizes(50000, 42);
    std::uintptr_t localSink = 0;
    Timer timer;
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(128);

    for (size_t size : sizes)
    {
        void* ptr = Allocator::allocate(size);
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
        if (live.size() > 64)
        {
            Allocator::deallocate(live.back().first, live.back().second);
            live.pop_back();
        }
    }

    for (const auto& [ptr, size] : live)
    {
        Allocator::deallocate(ptr, size);
    }

    const double elapsed = timer.elapsedMilliseconds();
    commitSink(localSink);
    return elapsed;
}

inline double benchmarkMixedSmallChurnSystem()
{
    const auto sizes = makeSmallSizes(50000, 42);
    std::uintptr_t localSink = 0;
    Timer timer;
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(128);

    for (size_t size : sizes)
    {
        char* ptr = new char[size];
        localSink += touchAllocation(ptr, size);
        live.push_back({ptr, size});
        if (live.size() > 64)
        {
            delete[] static_cast<char*>(live.back().first);
            live.pop_back();
        }
    }

    for (const auto& [ptr, size] : live)
    {
        (void)size;
        delete[] static_cast<char*>(ptr);
    }

    const double elapsed = timer.elapsedMilliseconds();
    commitSink(localSink);
    return elapsed;
}

template <typename Allocator>
double benchmarkCrossThreadFreeAllocator()
{
    const auto sizes = makeSmallSizes(50000, 84);
    std::vector<std::pair<void*, size_t>> objects(sizes.size());
    std::uintptr_t producerSink = 0;

    Timer timer;
    std::thread producer([&]()
    {
        for (size_t index = 0; index < sizes.size(); ++index)
        {
            const size_t size = sizes[index];
            void* ptr = Allocator::allocate(size);
            producerSink += touchAllocation(ptr, size);
            objects[index] = {ptr, size};
        }
    });
    producer.join();

    std::thread consumer([&]()
    {
        for (const auto& [ptr, size] : objects)
        {
            Allocator::deallocate(ptr, size);
        }
    });
    consumer.join();

    const double elapsed = timer.elapsedMilliseconds();
    commitSink(producerSink);
    return elapsed;
}

inline double benchmarkCrossThreadFreeSystem()
{
    const auto sizes = makeSmallSizes(50000, 84);
    std::vector<std::pair<void*, size_t>> objects(sizes.size());
    std::uintptr_t producerSink = 0;

    Timer timer;
    std::thread producer([&]()
    {
        for (size_t index = 0; index < sizes.size(); ++index)
        {
            const size_t size = sizes[index];
            char* ptr = new char[size];
            producerSink += touchAllocation(ptr, size);
            objects[index] = {ptr, size};
        }
    });
    producer.join();

    std::thread consumer([&]()
    {
        for (const auto& [ptr, size] : objects)
        {
            (void)size;
            delete[] static_cast<char*>(ptr);
        }
    });
    consumer.join();

    const double elapsed = timer.elapsedMilliseconds();
    commitSink(producerSink);
    return elapsed;
}

template <typename Allocator>
std::vector<ScenarioResult> runSuite()
{
    Allocator::initialize();

    const std::vector<std::pair<std::string, std::function<double()>>> allocatorBenchmarks = {
        {"single_thread_small", &benchmarkSingleThreadAllocator<Allocator>},
        {"multi_thread_small", &benchmarkMultiThreadAllocator<Allocator>},
        {"mixed_small_churn", &benchmarkMixedSmallChurnAllocator<Allocator>},
        {"cross_thread_free", &benchmarkCrossThreadFreeAllocator<Allocator>},
    };

    const std::vector<std::function<double()>> systemBenchmarks = {
        &benchmarkSingleThreadSystem,
        &benchmarkMultiThreadSystem,
        &benchmarkMixedSmallChurnSystem,
        &benchmarkCrossThreadFreeSystem,
    };

    std::vector<ScenarioResult> results;
    results.reserve(allocatorBenchmarks.size());
    for (size_t index = 0; index < allocatorBenchmarks.size(); ++index)
    {
        ScenarioResult result;
        result.name = allocatorBenchmarks[index].first;

        Allocator::cleanup();
        warmupAllocator<Allocator>();
        result.allocator_ms = runRepeated(allocatorBenchmarks[index].second);
        Allocator::cleanup();

        warmupSystem();
        result.system_ms = runRepeated(systemBenchmarks[index]);

        results.push_back(result);
    }
    return results;
}

template <typename Allocator>
int runMain()
{
    const auto results = runSuite<Allocator>();

    std::cout << "Unified benchmark for " << Allocator::label() << std::endl;
    std::cout << "Common workload note: all scenarios stay within 512 bytes so V1/V3/V4 remain on comparable small-object paths." << std::endl;
    for (const auto& result : results)
    {
        const double ratio = result.allocator_ms / result.system_ms;
        std::cout << std::fixed << std::setprecision(3)
                  << "RESULT|" << Allocator::label()
                  << "|" << result.name
                  << "|" << result.allocator_ms
                  << "|" << result.system_ms
                  << "|" << ratio
                  << std::endl;
    }
    std::cout << "SINK|" << benchmarkSink << std::endl;
    return 0;
}

} // namespace unified_bench
