#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace glock
{

constexpr size_t ALIGNMENT = 8;
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t POOL_SPAN_PAGES = 16;
constexpr size_t POOL_SPAN_BYTES = PAGE_SIZE * POOL_SPAN_PAGES;
constexpr size_t SMALL_ALLOC_MAX = 1024;
constexpr size_t RELEASE_HIGH_WATER_PAGES = 1024;
constexpr size_t RELEASE_LOW_WATER_PAGES = 512;
constexpr size_t SIZE_CLASS_COUNT = 24;

struct FreeBlock
{
    FreeBlock* next;
};

enum class PoolState
{
    Partial,
    Full,
    Empty
};

struct MemoryPoolStats
{
    size_t small_alloc_requests = 0;
    size_t large_alloc_requests = 0;
    size_t active_pool_count = 0;
    size_t empty_pool_count = 0;
    size_t cached_free_pages = 0;
    size_t os_reserved_bytes = 0;
    size_t os_released_bytes = 0;

    struct ThreadCacheRuntimeStats
    {
        size_t alloc_hits = 0;
        size_t alloc_misses = 0;
        size_t batch_fetches = 0;
        size_t blocks_fetched = 0;
        size_t batch_returns = 0;
        size_t blocks_returned = 0;
        size_t flush_calls = 0;
        size_t blocks_flushed = 0;
        size_t large_alloc_direct = 0;
        size_t large_free_direct = 0;
    } thread_cache;

    struct CentralCacheRuntimeStats
    {
        size_t acquire_calls = 0;
        size_t release_calls = 0;
        size_t blocks_acquired = 0;
        size_t blocks_released = 0;
        size_t partial_pool_hits = 0;
        size_t empty_pool_hits = 0;
        size_t pools_created = 0;
        size_t pools_released = 0;
        size_t scavenge_calls = 0;
    } central_cache;

    struct PageAllocatorRuntimeStats
    {
        size_t span_alloc_calls = 0;
        size_t span_free_calls = 0;
        size_t cache_hit_allocs = 0;
        size_t cache_miss_allocs = 0;
        size_t system_alloc_calls = 0;
        size_t system_free_calls = 0;
        size_t coalesce_merges = 0;
        size_t scavenge_calls = 0;
        size_t released_spans = 0;
        size_t immediately_freeable_pages = 0;
    } page_allocator;
};

struct MemoryPoolRuntimeTuning
{
    size_t batch_target_bytes = 24 * 1024;
    size_t batch_max_count = 96;
    size_t thread_cache_retain_batches = 4;
    size_t thread_cache_high_water_batches = 8;
    size_t central_empty_pools_small = 8;
    size_t central_empty_pools_medium = 4;
    size_t central_empty_pools_large = 2;
    size_t page_release_high_water_pages = 1024;
    size_t page_release_low_water_pages = 512;
    size_t page_auto_scavenge_trigger_pages = 4096;
};

class MemoryPoolTuning
{
public:
    static constexpr MemoryPoolRuntimeTuning defaults()
    {
        return {};
    }

    static MemoryPoolRuntimeTuning get()
    {
        MemoryPoolRuntimeTuning tuning;
        tuning.batch_target_bytes = batchTargetBytes_.load(std::memory_order_relaxed);
        tuning.batch_max_count = batchMaxCount_.load(std::memory_order_relaxed);
        tuning.thread_cache_retain_batches = threadCacheRetainBatches_.load(std::memory_order_relaxed);
        tuning.thread_cache_high_water_batches = threadCacheHighWaterBatches_.load(std::memory_order_relaxed);
        tuning.central_empty_pools_small = centralEmptyPoolsSmall_.load(std::memory_order_relaxed);
        tuning.central_empty_pools_medium = centralEmptyPoolsMedium_.load(std::memory_order_relaxed);
        tuning.central_empty_pools_large = centralEmptyPoolsLarge_.load(std::memory_order_relaxed);
        tuning.page_release_high_water_pages = pageReleaseHighWaterPages_.load(std::memory_order_relaxed);
        tuning.page_release_low_water_pages = pageReleaseLowWaterPages_.load(std::memory_order_relaxed);
        tuning.page_auto_scavenge_trigger_pages = pageAutoScavengeTriggerPages_.load(std::memory_order_relaxed);
        return tuning;
    }

    static void resetDefaults()
    {
        set(defaults());
    }

    static void set(const MemoryPoolRuntimeTuning& tuning)
    {
        const size_t batchTargetBytes = std::max<size_t>(ALIGNMENT, tuning.batch_target_bytes);
        const size_t batchMaxCount = std::max<size_t>(1, tuning.batch_max_count);
        const size_t retainBatches = std::max<size_t>(1, tuning.thread_cache_retain_batches);
        const size_t highWaterBatches = std::max<size_t>(retainBatches + 1, tuning.thread_cache_high_water_batches);
        const size_t releaseLowPages = std::max<size_t>(1, tuning.page_release_low_water_pages);
        const size_t releaseHighPages = std::max<size_t>(releaseLowPages, tuning.page_release_high_water_pages);
        const size_t autoScavengeTriggerPages = std::max<size_t>(releaseHighPages, tuning.page_auto_scavenge_trigger_pages);

        batchTargetBytes_.store(batchTargetBytes, std::memory_order_relaxed);
        batchMaxCount_.store(batchMaxCount, std::memory_order_relaxed);
        threadCacheRetainBatches_.store(retainBatches, std::memory_order_relaxed);
        threadCacheHighWaterBatches_.store(highWaterBatches, std::memory_order_relaxed);
        centralEmptyPoolsSmall_.store(tuning.central_empty_pools_small, std::memory_order_relaxed);
        centralEmptyPoolsMedium_.store(tuning.central_empty_pools_medium, std::memory_order_relaxed);
        centralEmptyPoolsLarge_.store(tuning.central_empty_pools_large, std::memory_order_relaxed);
        pageReleaseHighWaterPages_.store(releaseHighPages, std::memory_order_relaxed);
        pageReleaseLowWaterPages_.store(releaseLowPages, std::memory_order_relaxed);
        pageAutoScavengeTriggerPages_.store(autoScavengeTriggerPages, std::memory_order_relaxed);
    }

    static size_t getBatchCount(size_t classSize)
    {
        const size_t targetBytes = batchTargetBytes_.load(std::memory_order_relaxed);
        const size_t maxBatch = std::max<size_t>(1, targetBytes / classSize);
        return std::clamp(maxBatch, size_t(1), batchMaxCount_.load(std::memory_order_relaxed));
    }

    static size_t getThreadCacheRetainTarget(size_t batchCount)
    {
        return std::max<size_t>(1, batchCount * threadCacheRetainBatches_.load(std::memory_order_relaxed));
    }

    static size_t getThreadCacheHighWatermark(size_t batchCount)
    {
        return std::max<size_t>(batchCount + 1, batchCount * threadCacheHighWaterBatches_.load(std::memory_order_relaxed));
    }

    static size_t getRetainedEmptyPoolLimit(size_t classSize)
    {
        if (classSize <= 256)
        {
            return centralEmptyPoolsSmall_.load(std::memory_order_relaxed);
        }

        if (classSize <= 512)
        {
            return centralEmptyPoolsMedium_.load(std::memory_order_relaxed);
        }

        return centralEmptyPoolsLarge_.load(std::memory_order_relaxed);
    }

    static size_t getPageReleaseHighWaterPages()
    {
        return pageReleaseHighWaterPages_.load(std::memory_order_relaxed);
    }

    static size_t getPageReleaseLowWaterPages()
    {
        return pageReleaseLowWaterPages_.load(std::memory_order_relaxed);
    }

    static size_t getPageAutoScavengeTriggerPages()
    {
        return pageAutoScavengeTriggerPages_.load(std::memory_order_relaxed);
    }

private:
    inline static std::atomic<size_t> batchTargetBytes_{defaults().batch_target_bytes};
    inline static std::atomic<size_t> batchMaxCount_{defaults().batch_max_count};
    inline static std::atomic<size_t> threadCacheRetainBatches_{defaults().thread_cache_retain_batches};
    inline static std::atomic<size_t> threadCacheHighWaterBatches_{defaults().thread_cache_high_water_batches};
    inline static std::atomic<size_t> centralEmptyPoolsSmall_{defaults().central_empty_pools_small};
    inline static std::atomic<size_t> centralEmptyPoolsMedium_{defaults().central_empty_pools_medium};
    inline static std::atomic<size_t> centralEmptyPoolsLarge_{defaults().central_empty_pools_large};
    inline static std::atomic<size_t> pageReleaseHighWaterPages_{defaults().page_release_high_water_pages};
    inline static std::atomic<size_t> pageReleaseLowWaterPages_{defaults().page_release_low_water_pages};
    inline static std::atomic<size_t> pageAutoScavengeTriggerPages_{defaults().page_auto_scavenge_trigger_pages};
};

class SizeClass
{
public:
    inline static constexpr std::array<size_t, SIZE_CLASS_COUNT> kClassSizes = {
        8, 16, 24, 32, 40, 48, 56, 64,
        80, 96, 112, 128,
        160, 192, 224, 256,
        320, 384, 448, 512,
        640, 768, 896, 1024
    };

    static size_t getClassIndex(size_t bytes)
    {
        const size_t normalized = std::max(bytes, ALIGNMENT);
        const auto it = std::lower_bound(kClassSizes.begin(), kClassSizes.end(), normalized);
        return static_cast<size_t>(it - kClassSizes.begin());
    }

    static size_t getClassSize(size_t index)
    {
        return kClassSizes[index];
    }

    static size_t normalizeSize(size_t bytes)
    {
        if (bytes == 0)
        {
            bytes = ALIGNMENT;
        }

        if (bytes > SMALL_ALLOC_MAX)
        {
            return bytes;
        }

        return getClassSize(getClassIndex(bytes));
    }

    static size_t getBatchCount(size_t classSize)
    {
        return MemoryPoolTuning::getBatchCount(classSize);
    }
};

inline size_t toPageId(const void* ptr)
{
    return static_cast<size_t>(reinterpret_cast<std::uintptr_t>(ptr) / PAGE_SIZE);
}

inline size_t roundUpToPages(size_t bytes)
{
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

} // namespace glock
