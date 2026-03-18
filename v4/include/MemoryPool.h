#pragma once

#include "CentralCache.h"
#include "PageAllocator.h"
#include "ThreadCache.h"

#include <atomic>

namespace Kama_memoryPool
{

class MemoryPool
{
public:
    static void* allocate(size_t size)
    {
        const size_t normalized = (size == 0) ? ALIGNMENT : size;
        recordAllocationRequest(normalized <= SMALL_ALLOC_MAX);
        return ThreadCache::getInstance()->allocate(normalized);
    }

    static void deallocate(void* ptr, size_t size)
    {
        if (!ptr)
        {
            return;
        }

        ThreadCache::getInstance()->deallocate(ptr, size);
    }

    static void scavenge()
    {
        flushCurrentThreadStats();
        ThreadCache::getInstance()->flushAll();
        CentralCache::getInstance().scavenge(true);
        PageAllocator::getInstance().scavenge(true);
    }

    static MemoryPoolStats getStats()
    {
        flushCurrentThreadStats();

        MemoryPoolStats stats;
        stats.small_alloc_requests = smallAllocRequests_.load(std::memory_order_relaxed);
        stats.large_alloc_requests = largeAllocRequests_.load(std::memory_order_relaxed);
        stats.active_pool_count = CentralCache::getInstance().getActivePoolCount();
        stats.empty_pool_count = CentralCache::getInstance().getEmptyPoolCount();
        stats.cached_free_pages = PageAllocator::getInstance().getCachedFreePages();
        stats.os_reserved_bytes = PageAllocator::getInstance().getReservedBytes();
        stats.os_released_bytes = PageAllocator::getInstance().getReleasedBytes();
        return stats;
    }

private:
    struct ThreadStatsBuffer
    {
        size_t smallPending = 0;
        size_t largePending = 0;

        ~ThreadStatsBuffer()
        {
            MemoryPool::flushThreadStats(*this);
        }
    };

    static constexpr size_t STATS_FLUSH_THRESHOLD = 256;

    static ThreadStatsBuffer& currentThreadStats()
    {
        static thread_local ThreadStatsBuffer stats;
        return stats;
    }

    static void recordAllocationRequest(bool isSmall)
    {
        ThreadStatsBuffer& stats = currentThreadStats();
        size_t& pending = isSmall ? stats.smallPending : stats.largePending;
        ++pending;
        if (pending >= STATS_FLUSH_THRESHOLD)
        {
            flushThreadStats(stats);
        }
    }

    static void flushCurrentThreadStats()
    {
        flushThreadStats(currentThreadStats());
    }

    static void flushThreadStats(ThreadStatsBuffer& stats)
    {
        if (stats.smallPending != 0)
        {
            smallAllocRequests_.fetch_add(stats.smallPending, std::memory_order_relaxed);
            stats.smallPending = 0;
        }

        if (stats.largePending != 0)
        {
            largeAllocRequests_.fetch_add(stats.largePending, std::memory_order_relaxed);
            stats.largePending = 0;
        }
    }

private:
    inline static std::atomic<size_t> smallAllocRequests_{0};
    inline static std::atomic<size_t> largeAllocRequests_{0};
};

} // namespace Kama_memoryPool
