#include "../include/CentralCache.h"

#include "../include/PageAllocator.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace glock
{

namespace
{

PoolState computeState(uint32_t freeCount, uint32_t capacity)
{
    if (freeCount == capacity)
    {
        return PoolState::Empty;
    }

    if (freeCount == 0)
    {
        return PoolState::Full;
    }

    return PoolState::Partial;
}

} // namespace

CentralCache::CentralCache()
{
    for (size_t index = 0; index < SIZE_CLASS_COUNT; ++index)
    {
        tables_[index].size_class_bytes = SizeClass::getClassSize(index);
    }
}

FreeBlock* CentralCache::acquireBatch(size_t classIndex, size_t requestedCount, size_t& actualCount)
{
    actualCount = 0;
    acquireCalls_.fetch_add(1, std::memory_order_relaxed);
    if (classIndex >= SIZE_CLASS_COUNT || requestedCount == 0)
    {
        return nullptr;
    }

    PoolTable& table = tables_[classIndex];
    std::lock_guard<std::mutex> lock(table.lock);

    PoolInfo* pool = table.partial_pools;
    if (pool == nullptr)
    {
        pool = table.empty_pools;
        if (pool != nullptr)
        {
            emptyPoolHits_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else
    {
        partialPoolHits_.fetch_add(1, std::memory_order_relaxed);
    }

    if (pool == nullptr)
    {
        pool = createPoolLocked(classIndex, table);
    }

    if (pool == nullptr)
    {
        return nullptr;
    }

    FreeBlock* result = popBatchFromPoolLocked(table, pool, requestedCount, actualCount);
    if (actualCount != 0)
    {
        blocksAcquired_.fetch_add(actualCount, std::memory_order_relaxed);
    }
    return result;
}

void CentralCache::releaseBatch(size_t classIndex, FreeBlock* head, size_t count)
{
    releaseCalls_.fetch_add(1, std::memory_order_relaxed);
    if (classIndex >= SIZE_CLASS_COUNT || head == nullptr || count == 0)
    {
        return;
    }

    struct PendingPoolReturn
    {
        PoolInfo* pool = nullptr;
        FreeBlock* head = nullptr;
        FreeBlock* tail = nullptr;
        size_t count = 0;
    };

    std::vector<PendingPoolReturn> pendingReturns;
    pendingReturns.reserve(std::min<size_t>(count, 16));
    std::unordered_map<PoolInfo*, size_t> pendingIndexByPool;
    constexpr size_t INDEX_BUILD_THRESHOLD = 8;
    PendingPoolReturn* lastPending = nullptr;
    PoolInfo* lastPool = nullptr;
    bool useIndexedLookup = false;

    PageAllocator::getInstance().forEachSpanInBatch(
        head,
        count,
        [&](FreeBlock* current, PageAllocator::SpanInfo* span)
        {
            current->next = nullptr;

            PoolInfo* pool = (span != nullptr) ? static_cast<PoolInfo*>(span->owner) : nullptr;
            if (pool != nullptr && pool->size_class_index == classIndex)
            {
                if (pool == lastPool && lastPending != nullptr)
                {
                    lastPending->tail->next = current;
                    lastPending->tail = current;
                    ++lastPending->count;
                    return;
                }

                PendingPoolReturn* pending = nullptr;
                if (useIndexedLookup)
                {
                    auto it = pendingIndexByPool.find(pool);
                    if (it != pendingIndexByPool.end())
                    {
                        pending = &pendingReturns[it->second];
                    }
                }
                else
                {
                    for (PendingPoolReturn& entry : pendingReturns)
                    {
                        if (entry.pool == pool)
                        {
                            pending = &entry;
                            break;
                        }
                    }

                    if (pending == nullptr && pendingReturns.size() >= INDEX_BUILD_THRESHOLD)
                    {
                        pendingIndexByPool.reserve(std::min<size_t>(count, 32));
                        for (size_t index = 0; index < pendingReturns.size(); ++index)
                        {
                            pendingIndexByPool.emplace(pendingReturns[index].pool, index);
                        }
                        useIndexedLookup = true;
                    }
                }

                if (pending == nullptr)
                {
                    pendingReturns.push_back({pool, current, current, 1});
                    pending = &pendingReturns.back();
                    if (useIndexedLookup)
                    {
                        pendingIndexByPool.emplace(pool, pendingReturns.size() - 1);
                    }
                }
                else
                {
                    pending->tail->next = current;
                    pending->tail = current;
                    ++pending->count;
                }

                lastPool = pool;
                lastPending = pending;
            }
        });

    if (pendingReturns.empty())
    {
        blocksReleased_.fetch_add(count, std::memory_order_relaxed);
        return;
    }

    PoolTable& table = tables_[classIndex];
    std::lock_guard<std::mutex> lock(table.lock);

    for (const PendingPoolReturn& pending : pendingReturns)
    {
        if (pending.pool == nullptr || pending.head == nullptr)
        {
            continue;
        }

        pending.tail->next = pending.pool->free_list;
        pending.pool->free_list = pending.head;
        pending.pool->free_count = static_cast<uint32_t>(pending.pool->free_count + pending.count);
        setPoolStateLocked(table, pending.pool, computeState(pending.pool->free_count, pending.pool->capacity));
    }

    blocksReleased_.fetch_add(count, std::memory_order_relaxed);
}

void CentralCache::scavenge(bool force)
{
    scavengeCalls_.fetch_add(1, std::memory_order_relaxed);
    for (size_t index = 0; index < SIZE_CLASS_COUNT; ++index)
    {
        PoolTable& table = tables_[index];
        std::lock_guard<std::mutex> lock(table.lock);
        releaseEmptyPoolsLocked(table, force);
    }
}

size_t CentralCache::getActivePoolCount() const
{
    return activePoolCount_.load(std::memory_order_relaxed);
}

size_t CentralCache::getEmptyPoolCount() const
{
    return emptyPoolCount_.load(std::memory_order_relaxed);
}

MemoryPoolStats::CentralCacheRuntimeStats CentralCache::getRuntimeStats() const
{
    MemoryPoolStats::CentralCacheRuntimeStats stats;
    stats.acquire_calls = acquireCalls_.load(std::memory_order_relaxed);
    stats.release_calls = releaseCalls_.load(std::memory_order_relaxed);
    stats.blocks_acquired = blocksAcquired_.load(std::memory_order_relaxed);
    stats.blocks_released = blocksReleased_.load(std::memory_order_relaxed);
    stats.partial_pool_hits = partialPoolHits_.load(std::memory_order_relaxed);
    stats.empty_pool_hits = emptyPoolHits_.load(std::memory_order_relaxed);
    stats.pools_created = poolsCreated_.load(std::memory_order_relaxed);
    stats.pools_released = poolsReleased_.load(std::memory_order_relaxed);
    stats.scavenge_calls = scavengeCalls_.load(std::memory_order_relaxed);
    return stats;
}

size_t CentralCache::getRetainedEmptyPoolLimit(const PoolTable& table) const
{
    return MemoryPoolTuning::getRetainedEmptyPoolLimit(table.size_class_bytes);
}

CentralCache::PoolInfo* CentralCache::createPoolLocked(size_t classIndex, PoolTable& table)
{
    void* span = PageAllocator::getInstance().allocateSpan(POOL_SPAN_PAGES, true);
    if (span == nullptr)
    {
        return nullptr;
    }

    const size_t classSize = table.size_class_bytes;
    const uint32_t capacity = static_cast<uint32_t>(POOL_SPAN_BYTES / classSize);
    if (capacity == 0)
    {
        PageAllocator::getInstance().deallocateSpan(span, POOL_SPAN_PAGES);
        return nullptr;
    }

    FreeBlock* head = nullptr;
    FreeBlock* tail = nullptr;
    for (uint32_t blockIndex = 0; blockIndex < capacity; ++blockIndex)
    {
        auto* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(span) + blockIndex * classSize);
        block->next = nullptr;
        if (head == nullptr)
        {
            head = block;
            tail = block;
        }
        else
        {
            tail->next = block;
            tail = block;
        }
    }

    PoolInfo* pool = new PoolInfo;
    pool->span_addr = span;
    pool->free_list = head;
    pool->capacity = capacity;
    pool->free_count = capacity;
    pool->size_class_index = static_cast<uint16_t>(classIndex);
    pool->state = PoolState::Empty;
    addPoolToList(table.empty_pools, pool);
    ++table.empty_pool_count;
    emptyPoolCount_.fetch_add(1, std::memory_order_relaxed);
    poolsCreated_.fetch_add(1, std::memory_order_relaxed);
    PageAllocator::getInstance().setSpanOwner(pool->span_addr, pool);
    return pool;
}

FreeBlock* CentralCache::popBatchFromPoolLocked(PoolTable& table, PoolInfo* pool, size_t requestedCount, size_t& actualCount)
{
    actualCount = std::min<size_t>(requestedCount, pool->free_count);
    if (actualCount == 0)
    {
        return nullptr;
    }

    FreeBlock* head = pool->free_list;
    FreeBlock* tail = head;
    for (size_t index = 1; index < actualCount; ++index)
    {
        tail = tail->next;
    }

    pool->free_list = tail->next;
    tail->next = nullptr;
    pool->free_count = static_cast<uint32_t>(pool->free_count - actualCount);
    setPoolStateLocked(table, pool, computeState(pool->free_count, pool->capacity));
    return head;
}

void CentralCache::pushBlockToPoolLocked(PoolTable& table, PoolInfo* pool, FreeBlock* block)
{
    block->next = pool->free_list;
    pool->free_list = block;
    pool->free_count = static_cast<uint32_t>(pool->free_count + 1);
    setPoolStateLocked(table, pool, computeState(pool->free_count, pool->capacity));
}

void CentralCache::setPoolStateLocked(PoolTable& table, PoolInfo* pool, PoolState newState)
{
    if (pool->state == newState)
    {
        return;
    }

    auto removeFromState = [&](PoolState state)
    {
        switch (state)
        {
        case PoolState::Partial:
            removePoolFromList(table.partial_pools, pool);
            activePoolCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        case PoolState::Full:
            removePoolFromList(table.full_pools, pool);
            activePoolCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        case PoolState::Empty:
            removePoolFromList(table.empty_pools, pool);
            if (table.empty_pool_count > 0)
            {
                --table.empty_pool_count;
            }
            emptyPoolCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        }
    };

    auto addToState = [&](PoolState state)
    {
        switch (state)
        {
        case PoolState::Partial:
            addPoolToList(table.partial_pools, pool);
            activePoolCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PoolState::Full:
            addPoolToList(table.full_pools, pool);
            activePoolCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PoolState::Empty:
            addPoolToList(table.empty_pools, pool);
            ++table.empty_pool_count;
            emptyPoolCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    };

    removeFromState(pool->state);
    pool->state = newState;
    addToState(newState);
}

void CentralCache::addPoolToList(PoolInfo*& head, PoolInfo* pool)
{
    pool->prev = nullptr;
    pool->next = head;
    if (head != nullptr)
    {
        head->prev = pool;
    }
    head = pool;
}

void CentralCache::removePoolFromList(PoolInfo*& head, PoolInfo* pool)
{
    if (pool->prev != nullptr)
    {
        pool->prev->next = pool->next;
    }
    else
    {
        head = pool->next;
    }

    if (pool->next != nullptr)
    {
        pool->next->prev = pool->prev;
    }

    pool->prev = nullptr;
    pool->next = nullptr;
}

void CentralCache::releasePoolLocked(PoolTable& table, PoolInfo* pool)
{
    removePoolFromList(table.empty_pools, pool);
    if (table.empty_pool_count > 0)
    {
        --table.empty_pool_count;
    }
    emptyPoolCount_.fetch_sub(1, std::memory_order_relaxed);
    poolsReleased_.fetch_add(1, std::memory_order_relaxed);
    PageAllocator::getInstance().setSpanOwner(pool->span_addr, nullptr);
    PageAllocator::getInstance().deallocateSpan(pool->span_addr, POOL_SPAN_PAGES);
    delete pool;
}

void CentralCache::releaseEmptyPoolsLocked(PoolTable& table, bool force)
{
    if (!force)
    {
        const size_t retainedLimit = getRetainedEmptyPoolLimit(table);
        if (table.empty_pool_count <= retainedLimit)
        {
            return;
        }
    }

    PoolInfo* current = table.empty_pools;
    if (!force)
    {
        const size_t retainedLimit = getRetainedEmptyPoolLimit(table);
        for (size_t retained = 0; retained < retainedLimit && current != nullptr; ++retained)
        {
            current = current->next;
        }
    }

    while (current != nullptr)
    {
        PoolInfo* next = current->next;
        releasePoolLocked(table, current);
        current = next;
    }
}

} // namespace glock
