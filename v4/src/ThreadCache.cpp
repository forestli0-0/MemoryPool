#include "../include/ThreadCache.h"

#include "../include/CentralCache.h"
#include "../include/PageAllocator.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace glock
{

namespace
{

#if defined(_WIN32) && defined(__GNUC__)
__thread ThreadCache* gCurrentThreadCache = nullptr;
#elif defined(_WIN32)
__declspec(thread) ThreadCache* gCurrentThreadCache = nullptr;
#else
thread_local ThreadCache* gCurrentThreadCache = nullptr;
#endif

void destroyThreadCache(ThreadCache* cache)
{
    if (cache == nullptr)
    {
        return;
    }

    delete cache;
}

#ifdef _WIN32
VOID CALLBACK destroyThreadCacheForThreadExit(PVOID value)
{
    destroyThreadCache(static_cast<ThreadCache*>(value));
}

DWORD getThreadCacheTlsKey()
{
    static const DWORD key = FlsAlloc(destroyThreadCacheForThreadExit);
    return key;
}

void registerThreadCacheForCleanup(ThreadCache* cache)
{
    const DWORD key = getThreadCacheTlsKey();
    if (key != FLS_OUT_OF_INDEXES)
    {
        FlsSetValue(key, cache);
    }
}
#else
void destroyThreadCacheForThreadExit(void* value)
{
    destroyThreadCache(static_cast<ThreadCache*>(value));
}

pthread_key_t getThreadCacheTlsKey()
{
    static const pthread_key_t key = []()
    {
        pthread_key_t createdKey = {};
        pthread_key_create(&createdKey, destroyThreadCacheForThreadExit);
        return createdKey;
    }();
    return key;
}

void registerThreadCacheForCleanup(ThreadCache* cache)
{
    pthread_setspecific(getThreadCacheTlsKey(), cache);
}
#endif

} // namespace

ThreadCache* ThreadCache::getInstance()
{
    ThreadCache*& cache = currentThreadCachePointer();
    if (cache == nullptr)
    {
        cache = new ThreadCache();
        registerThreadCacheForCleanup(cache);
    }

    return cache;
}

ThreadCache* ThreadCache::getCurrentThreadIfAny()
{
    return currentThreadCachePointer();
}

size_t ThreadCache::getSmallAllocRequestCount()
{
    return smallAllocRequests_.load(std::memory_order_relaxed);
}

size_t ThreadCache::getLargeAllocRequestCount()
{
    return largeAllocRequests_.load(std::memory_order_relaxed);
}

ThreadCache*& ThreadCache::currentThreadCachePointer()
{
    return gCurrentThreadCache;
}

ThreadCache::ThreadCache()
{
    freeLists_.fill(nullptr);
    counts_.fill(0);
}

ThreadCache::~ThreadCache()
{
    flushAll();
    flushLocalStats();
}

void* ThreadCache::allocate(size_t size)
{
    const size_t normalized = (size == 0) ? ALIGNMENT : size;
    hasSeenAllocations_ = true;
    recordAllocationRequest(normalized <= SMALL_ALLOC_MAX);
    if (normalized > SMALL_ALLOC_MAX)
    {
        ++localStats_.large_alloc_direct;
        return PageAllocator::getInstance().allocateSpan(roundUpToPages(normalized), false);
    }

    const size_t classIndex = SizeClass::getClassIndex(normalized);
    if (freeLists_[classIndex] == nullptr)
    {
        ++localStats_.alloc_misses;
        fetchBatch(classIndex);
    }
    else
    {
        ++localStats_.alloc_hits;
    }

    FreeBlock* block = popLocal(classIndex);
    return block;
}

void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (ptr == nullptr)
    {
        return;
    }

    const size_t normalized = (size == 0) ? ALIGNMENT : size;
    if (normalized > SMALL_ALLOC_MAX)
    {
        ++localStats_.large_free_direct;
        PageAllocator::getInstance().deallocateSpan(ptr, roundUpToPages(normalized));
        return;
    }

    const size_t classIndex = SizeClass::getClassIndex(normalized);
    pushLocal(classIndex, static_cast<FreeBlock*>(ptr));

    const size_t batchCount = SizeClass::getBatchCount(SizeClass::getClassSize(classIndex));
    size_t retainTarget = MemoryPoolTuning::getThreadCacheRetainTarget(batchCount);
    size_t highWatermark = MemoryPoolTuning::getThreadCacheHighWatermark(batchCount);
    if (!hasSeenAllocations_)
    {
        // Deallocate-only threads are unlikely to reuse local blocks before exit,
        // so keep their cache shallow and return batches to central sooner.
        retainTarget = batchCount;
        highWatermark = batchCount * 2;
    }

    if (counts_[classIndex] > highWatermark)
    {
        returnBatch(classIndex, counts_[classIndex] - retainTarget);
    }
}

void ThreadCache::flushAll()
{
    size_t totalFlushed = 0;
    for (size_t classIndex = 0; classIndex < SIZE_CLASS_COUNT; ++classIndex)
    {
        if (freeLists_[classIndex] == nullptr || counts_[classIndex] == 0)
        {
            continue;
        }

        FreeBlock* head = freeLists_[classIndex];
        const size_t count = counts_[classIndex];
        freeLists_[classIndex] = nullptr;
        counts_[classIndex] = 0;
        totalFlushed += count;
        CentralCache::getInstance().releaseBatch(classIndex, head, count);
    }

    if (totalFlushed != 0)
    {
        ++localStats_.flush_calls;
        localStats_.blocks_flushed += totalFlushed;
    }
}

void ThreadCache::flushCurrentThreadStats()
{
    if (ThreadCache* cache = getCurrentThreadIfAny())
    {
        cache->flushLocalStats();
    }
}

MemoryPoolStats::ThreadCacheRuntimeStats ThreadCache::getRuntimeStats()
{
    MemoryPoolStats::ThreadCacheRuntimeStats stats;
    stats.alloc_hits = allocHits_.load(std::memory_order_relaxed);
    stats.alloc_misses = allocMisses_.load(std::memory_order_relaxed);
    stats.batch_fetches = batchFetches_.load(std::memory_order_relaxed);
    stats.blocks_fetched = blocksFetched_.load(std::memory_order_relaxed);
    stats.batch_returns = batchReturns_.load(std::memory_order_relaxed);
    stats.blocks_returned = blocksReturned_.load(std::memory_order_relaxed);
    stats.flush_calls = flushCalls_.load(std::memory_order_relaxed);
    stats.blocks_flushed = blocksFlushed_.load(std::memory_order_relaxed);
    stats.large_alloc_direct = largeAllocDirect_.load(std::memory_order_relaxed);
    stats.large_free_direct = largeFreeDirect_.load(std::memory_order_relaxed);
    return stats;
}

void ThreadCache::flushLocalStats()
{
    if (localStats_.alloc_hits != 0)
    {
        allocHits_.fetch_add(localStats_.alloc_hits, std::memory_order_relaxed);
        localStats_.alloc_hits = 0;
    }

    if (localStats_.alloc_misses != 0)
    {
        allocMisses_.fetch_add(localStats_.alloc_misses, std::memory_order_relaxed);
        localStats_.alloc_misses = 0;
    }

    if (localStats_.batch_fetches != 0)
    {
        batchFetches_.fetch_add(localStats_.batch_fetches, std::memory_order_relaxed);
        localStats_.batch_fetches = 0;
    }

    if (localStats_.blocks_fetched != 0)
    {
        blocksFetched_.fetch_add(localStats_.blocks_fetched, std::memory_order_relaxed);
        localStats_.blocks_fetched = 0;
    }

    if (localStats_.batch_returns != 0)
    {
        batchReturns_.fetch_add(localStats_.batch_returns, std::memory_order_relaxed);
        localStats_.batch_returns = 0;
    }

    if (localStats_.blocks_returned != 0)
    {
        blocksReturned_.fetch_add(localStats_.blocks_returned, std::memory_order_relaxed);
        localStats_.blocks_returned = 0;
    }

    if (localStats_.flush_calls != 0)
    {
        flushCalls_.fetch_add(localStats_.flush_calls, std::memory_order_relaxed);
        localStats_.flush_calls = 0;
    }

    if (localStats_.blocks_flushed != 0)
    {
        blocksFlushed_.fetch_add(localStats_.blocks_flushed, std::memory_order_relaxed);
        localStats_.blocks_flushed = 0;
    }

    if (localStats_.large_alloc_direct != 0)
    {
        largeAllocDirect_.fetch_add(localStats_.large_alloc_direct, std::memory_order_relaxed);
        localStats_.large_alloc_direct = 0;
    }

    if (localStats_.large_free_direct != 0)
    {
        largeFreeDirect_.fetch_add(localStats_.large_free_direct, std::memory_order_relaxed);
        localStats_.large_free_direct = 0;
    }

    if (localStats_.small_alloc_requests != 0)
    {
        smallAllocRequests_.fetch_add(localStats_.small_alloc_requests, std::memory_order_relaxed);
        localStats_.small_alloc_requests = 0;
    }

    if (localStats_.large_alloc_requests != 0)
    {
        largeAllocRequests_.fetch_add(localStats_.large_alloc_requests, std::memory_order_relaxed);
        localStats_.large_alloc_requests = 0;
    }
}

void ThreadCache::recordAllocationRequest(bool isSmall)
{
    size_t& pending = isSmall ? localStats_.small_alloc_requests : localStats_.large_alloc_requests;
    ++pending;

    constexpr size_t REQUEST_STATS_FLUSH_THRESHOLD = 256;
    if (pending >= REQUEST_STATS_FLUSH_THRESHOLD)
    {
        flushLocalStats();
    }
}

FreeBlock* ThreadCache::popLocal(size_t classIndex)
{
    FreeBlock* head = freeLists_[classIndex];
    if (head == nullptr)
    {
        return nullptr;
    }

    freeLists_[classIndex] = head->next;
    head->next = nullptr;
    if (counts_[classIndex] > 0)
    {
        --counts_[classIndex];
    }
    return head;
}

void ThreadCache::pushLocal(size_t classIndex, FreeBlock* block)
{
    block->next = freeLists_[classIndex];
    freeLists_[classIndex] = block;
    ++counts_[classIndex];
}

void ThreadCache::fetchBatch(size_t classIndex)
{
    ++localStats_.batch_fetches;
    const size_t classSize = SizeClass::getClassSize(classIndex);
    const size_t requestedCount = SizeClass::getBatchCount(classSize);
    size_t actualCount = 0;
    FreeBlock* batch = CentralCache::getInstance().acquireBatch(classIndex, requestedCount, actualCount);
    if (batch == nullptr || actualCount == 0)
    {
        return;
    }

    freeLists_[classIndex] = batch;
    counts_[classIndex] = actualCount;
    localStats_.blocks_fetched += actualCount;
}

void ThreadCache::returnBatch(size_t classIndex, size_t batchCount)
{
    if (freeLists_[classIndex] == nullptr || batchCount == 0)
    {
        return;
    }

    const size_t actualCount = std::min(batchCount, counts_[classIndex]);
    FreeBlock* head = freeLists_[classIndex];
    FreeBlock* tail = head;
    for (size_t index = 1; index < actualCount; ++index)
    {
        tail = tail->next;
    }

    freeLists_[classIndex] = tail->next;
    tail->next = nullptr;
    counts_[classIndex] -= actualCount;
    ++localStats_.batch_returns;
    localStats_.blocks_returned += actualCount;
    CentralCache::getInstance().releaseBatch(classIndex, head, actualCount);
}

} // namespace glock
