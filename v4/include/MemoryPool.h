#pragma once

#include "CentralCache.h"
#include "PageAllocator.h"
#include "ThreadCache.h"

namespace glock
{

class MemoryPool
{
public:
    static MemoryPoolRuntimeTuning getTuning()
    {
        return MemoryPoolTuning::get();
    }

    static void applyTuning(const MemoryPoolRuntimeTuning& tuning)
    {
        scavenge();
        MemoryPoolTuning::set(tuning);
    }

    static void resetTuning()
    {
        applyTuning(MemoryPoolTuning::defaults());
    }

    static void* allocate(size_t size)
    {
        const size_t normalized = (size == 0) ? ALIGNMENT : size;
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
        ThreadCache::flushCurrentThreadStats();
        ThreadCache::getInstance()->flushAll();
        CentralCache::getInstance().scavenge(true);
        PageAllocator::getInstance().scavenge(true);
    }

    static MemoryPoolStats getStats()
    {
        ThreadCache::flushCurrentThreadStats();

        MemoryPoolStats stats;
        stats.small_alloc_requests = ThreadCache::getSmallAllocRequestCount();
        stats.large_alloc_requests = ThreadCache::getLargeAllocRequestCount();
        stats.active_pool_count = CentralCache::getInstance().getActivePoolCount();
        stats.empty_pool_count = CentralCache::getInstance().getEmptyPoolCount();
        stats.cached_free_pages = PageAllocator::getInstance().getCachedFreePages();
        stats.os_reserved_bytes = PageAllocator::getInstance().getReservedBytes();
        stats.os_released_bytes = PageAllocator::getInstance().getReleasedBytes();
        stats.thread_cache = ThreadCache::getRuntimeStats();
        stats.central_cache = CentralCache::getInstance().getRuntimeStats();
        stats.page_allocator = PageAllocator::getInstance().getRuntimeStats();
        return stats;
    }
};

} // namespace glock
