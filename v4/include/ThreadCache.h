#pragma once

#include "Common.h"

#include <atomic>
#include <array>

namespace glock
{

class ThreadCache
{
public:
    static ThreadCache* getInstance();
    static ThreadCache* getCurrentThreadIfAny();
    static size_t getSmallAllocRequestCount();
    static size_t getLargeAllocRequestCount();

    ~ThreadCache();

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);
    void flushAll();

    static void flushCurrentThreadStats();
    static MemoryPoolStats::ThreadCacheRuntimeStats getRuntimeStats();

private:
    struct LocalStats
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
        size_t small_alloc_requests = 0;
        size_t large_alloc_requests = 0;
    };

    ThreadCache();
    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;

    void recordAllocationRequest(bool isSmall);
    static ThreadCache*& currentThreadCachePointer();
    void flushLocalStats();
    FreeBlock* popLocal(size_t classIndex);
    void pushLocal(size_t classIndex, FreeBlock* block);
    void fetchBatch(size_t classIndex);
    void returnBatch(size_t classIndex, size_t batchCount);

private:
    std::array<FreeBlock*, SIZE_CLASS_COUNT> freeLists_;
    std::array<size_t, SIZE_CLASS_COUNT> counts_;
    LocalStats localStats_;
    bool hasSeenAllocations_ = false;

    inline static std::atomic<size_t> allocHits_{0};
    inline static std::atomic<size_t> allocMisses_{0};
    inline static std::atomic<size_t> batchFetches_{0};
    inline static std::atomic<size_t> blocksFetched_{0};
    inline static std::atomic<size_t> batchReturns_{0};
    inline static std::atomic<size_t> blocksReturned_{0};
    inline static std::atomic<size_t> flushCalls_{0};
    inline static std::atomic<size_t> blocksFlushed_{0};
    inline static std::atomic<size_t> largeAllocDirect_{0};
    inline static std::atomic<size_t> largeFreeDirect_{0};
    inline static std::atomic<size_t> smallAllocRequests_{0};
    inline static std::atomic<size_t> largeAllocRequests_{0};
};

} // namespace glock
