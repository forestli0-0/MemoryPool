#include "../include/MemoryPool.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace Kama_memoryPool;

namespace
{

volatile std::uintptr_t benchmarkSink = 0;

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

void warmup()
{
    for (size_t index = 0; index < 5000; ++index)
    {
        void* ptr = MemoryPool::allocate(64);
        MemoryPool::deallocate(ptr, 64);
    }
    MemoryPool::scavenge();
}

void runSingleThreadBenchmark()
{
    std::cout << "\n=== Single Thread Benchmark ===" << std::endl;
    constexpr size_t iterations = 200000;
    constexpr size_t sizes[] = {16, 64, 256, 1024};

    {
        std::uintptr_t localSink = 0;
        Timer timer;
        for (size_t index = 0; index < iterations; ++index)
        {
            const size_t size = sizes[index % 4];
            void* ptr = MemoryPool::allocate(size);
            localSink += touchAllocation(ptr, size);
            MemoryPool::deallocate(ptr, size);
        }
        const double elapsed = timer.elapsedMilliseconds();
        MemoryPool::scavenge();
        commitBenchmarkSink(localSink);
        std::cout << "V4 MemoryPool: " << std::fixed << std::setprecision(3)
                  << elapsed << " ms" << std::endl;
    }

    {
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
        commitBenchmarkSink(localSink);
        std::cout << "new/delete:    " << std::fixed << std::setprecision(3)
                  << elapsed << " ms" << std::endl;
    }
}

void runMultiThreadBenchmark()
{
    std::cout << "\n=== Multi Thread Benchmark ===" << std::endl;
    constexpr size_t threadCount = 4;
    constexpr size_t iterations = 100000;
    constexpr size_t sizes[] = {24, 96, 384, 1024};

    {
        std::vector<std::uintptr_t> threadSinks(threadCount, 0);
        Timer timer;
        std::vector<std::thread> threads;
        for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
        {
            threads.emplace_back([threadIndex, &sizes, &threadSinks]()
            {
                std::uintptr_t localSink = 0;
                for (size_t index = 0; index < iterations; ++index)
                {
                    const size_t size = sizes[(index + threadIndex) % 4];
                    void* ptr = MemoryPool::allocate(size);
                    localSink += touchAllocation(ptr, size);
                    MemoryPool::deallocate(ptr, size);
                }
                threadSinks[threadIndex] = localSink;
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        const double elapsed = timer.elapsedMilliseconds();
        MemoryPool::scavenge();
        std::uintptr_t totalSink = 0;
        for (std::uintptr_t sink : threadSinks)
        {
            totalSink += sink;
        }
        commitBenchmarkSink(totalSink);
        std::cout << "V4 MemoryPool: " << std::fixed << std::setprecision(3)
                  << elapsed << " ms" << std::endl;
    }

    {
        std::vector<std::uintptr_t> threadSinks(threadCount, 0);
        Timer timer;
        std::vector<std::thread> threads;
        for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
        {
            threads.emplace_back([threadIndex, &sizes, &threadSinks]()
            {
                std::uintptr_t localSink = 0;
                for (size_t index = 0; index < iterations; ++index)
                {
                    const size_t size = sizes[(index + threadIndex) % 4];
                    char* ptr = new char[size];
                    localSink += touchAllocation(ptr, size);
                    delete[] ptr;
                }
                threadSinks[threadIndex] = localSink;
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        const double elapsed = timer.elapsedMilliseconds();
        std::uintptr_t totalSink = 0;
        for (std::uintptr_t sink : threadSinks)
        {
            totalSink += sink;
        }
        commitBenchmarkSink(totalSink);
        std::cout << "new/delete:    " << std::fixed << std::setprecision(3)
                  << elapsed << " ms" << std::endl;
    }
}

void runMixedSizeBenchmark()
{
    std::cout << "\n=== Mixed Size Benchmark ===" << std::endl;
    constexpr size_t iterations = 50000;

    auto runMemoryPoolBenchmark = [iterations]()
    {
        std::mt19937 generator(42);
        std::uniform_int_distribution<size_t> smallDistribution(1, SMALL_ALLOC_MAX);
        std::uniform_int_distribution<size_t> largeDistribution(SMALL_ALLOC_MAX + 1, SMALL_ALLOC_MAX * 8);

        std::uintptr_t localSink = 0;
        Timer timer;
        std::vector<std::pair<void*, size_t>> liveAllocations;
        liveAllocations.reserve(iterations / 4);
        for (size_t index = 0; index < iterations; ++index)
        {
            const bool useLarge = (index % 10) == 0;
            const size_t size = useLarge ? largeDistribution(generator) : smallDistribution(generator);
            void* ptr = MemoryPool::allocate(size);
            localSink += touchAllocation(ptr, size);
            liveAllocations.push_back({ptr, size});

            if (liveAllocations.size() > 64)
            {
                MemoryPool::deallocate(liveAllocations.back().first, liveAllocations.back().second);
                liveAllocations.pop_back();
            }
        }

        for (const auto& [ptr, size] : liveAllocations)
        {
            MemoryPool::deallocate(ptr, size);
        }

        const double elapsed = timer.elapsedMilliseconds();
        MemoryPool::scavenge();
        commitBenchmarkSink(localSink);
        return elapsed;
    };

    auto runNewDeleteBenchmark = [iterations]()
    {
        std::mt19937 generator(42);
        std::uniform_int_distribution<size_t> smallDistribution(1, SMALL_ALLOC_MAX);
        std::uniform_int_distribution<size_t> largeDistribution(SMALL_ALLOC_MAX + 1, SMALL_ALLOC_MAX * 8);

        std::uintptr_t localSink = 0;
        Timer timer;
        std::vector<std::pair<void*, size_t>> liveAllocations;
        liveAllocations.reserve(iterations / 4);
        for (size_t index = 0; index < iterations; ++index)
        {
            const bool useLarge = (index % 10) == 0;
            const size_t size = useLarge ? largeDistribution(generator) : smallDistribution(generator);
            char* ptr = new char[size];
            localSink += touchAllocation(ptr, size);
            liveAllocations.push_back({ptr, size});

            if (liveAllocations.size() > 64)
            {
                delete[] static_cast<char*>(liveAllocations.back().first);
                liveAllocations.pop_back();
            }
        }

        for (const auto& [ptr, size] : liveAllocations)
        {
            (void)size;
            delete[] static_cast<char*>(ptr);
        }

        const double elapsed = timer.elapsedMilliseconds();
        commitBenchmarkSink(localSink);
        return elapsed;
    };

    std::cout << "V4 MemoryPool: " << std::fixed << std::setprecision(3)
              << runMemoryPoolBenchmark() << " ms" << std::endl;
    std::cout << "new/delete:    " << std::fixed << std::setprecision(3)
              << runNewDeleteBenchmark() << " ms" << std::endl;
}

void runComparableBenchmark()
{
    std::cout << "\n=== V1/V3 Comparable Shape ===" << std::endl;
    constexpr size_t sizes[] = {4, 20, 40, 80};
    constexpr size_t rounds = 10;
    constexpr size_t iterations = 100000;

    {
        std::uintptr_t localSink = 0;
        Timer timer;
        for (size_t round = 0; round < rounds; ++round)
        {
            for (size_t index = 0; index < iterations; ++index)
            {
                for (size_t size : sizes)
                {
                    void* ptr = MemoryPool::allocate(size);
                    localSink += touchAllocation(ptr, size);
                    MemoryPool::deallocate(ptr, size);
                }
            }
        }
        const double elapsed = timer.elapsedMilliseconds();
        MemoryPool::scavenge();
        commitBenchmarkSink(localSink);
        std::cout << "V4 MemoryPool: " << std::fixed << std::setprecision(3)
                  << elapsed << " ms" << std::endl;
    }

    {
        std::uintptr_t localSink = 0;
        Timer timer;
        for (size_t round = 0; round < rounds; ++round)
        {
            for (size_t index = 0; index < iterations; ++index)
            {
                for (size_t size : sizes)
                {
                    char* ptr = new char[size];
                    localSink += touchAllocation(ptr, size);
                    delete[] ptr;
                }
            }
        }
        const double elapsed = timer.elapsedMilliseconds();
        commitBenchmarkSink(localSink);
        std::cout << "new/delete:    " << std::fixed << std::setprecision(3)
                  << elapsed << " ms" << std::endl;
    }

    std::cout << "For cross-version numbers, run the existing v1 and v3 perf tests in their own directories." << std::endl;
}

} // namespace

int main()
{
    std::cout << "Starting V4 performance tests..." << std::endl;
    warmup();
    runSingleThreadBenchmark();
    runMultiThreadBenchmark();
    runMixedSizeBenchmark();
    runComparableBenchmark();
    std::cout << "Benchmark sink: " << benchmarkSink << std::endl;
    return 0;
}
