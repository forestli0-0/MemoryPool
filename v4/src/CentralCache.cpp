#include "../include/CentralCache.h"

#include "../include/PageAllocator.h"

#include <algorithm>
#include <vector>

namespace Kama_memoryPool
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
    }

    if (pool == nullptr)
    {
        pool = createPoolLocked(classIndex, table);
    }

    if (pool == nullptr)
    {
        return nullptr;
    }

    return popBatchFromPoolLocked(table, pool, requestedCount, actualCount);
}

void CentralCache::releaseBatch(size_t classIndex, FreeBlock* head, size_t count)
{
    if (classIndex >= SIZE_CLASS_COUNT || head == nullptr || count == 0)
    {
        return;
    }

    PoolTable& table = tables_[classIndex];
    std::lock_guard<std::mutex> lock(table.lock);

    struct PendingPoolReturn
    {
        PoolInfo* pool = nullptr;
        FreeBlock* head = nullptr;
        FreeBlock* tail = nullptr;
        size_t count = 0;
    };

    std::vector<PendingPoolReturn> pendingReturns;
    pendingReturns.reserve(std::min<size_t>(count, 8));

    PageAllocator::getInstance().forEachSpanInBatch(
        head,
        count,
        [&](FreeBlock* current, PageAllocator::SpanInfo* span)
        {
            current->next = nullptr;

            PoolInfo* pool = (span != nullptr) ? static_cast<PoolInfo*>(span->owner) : nullptr;
            if (pool != nullptr && pool->size_class_index == classIndex)
            {
                PendingPoolReturn* pending = nullptr;
                for (PendingPoolReturn& entry : pendingReturns)
                {
                    if (entry.pool == pool)
                    {
                        pending = &entry;
                        break;
                    }
                }

                if (pending == nullptr)
                {
                    pendingReturns.push_back({pool, current, current, 1});
                }
                else
                {
                    pending->tail->next = current;
                    pending->tail = current;
                    ++pending->count;
                }
            }
        });

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

    releaseEmptyPoolsLocked(table, false);
}

void CentralCache::scavenge(bool force)
{
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
    emptyPoolCount_.fetch_add(1, std::memory_order_relaxed);
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
    emptyPoolCount_.fetch_sub(1, std::memory_order_relaxed);
    PageAllocator::getInstance().setSpanOwner(pool->span_addr, nullptr);
    PageAllocator::getInstance().deallocateSpan(pool->span_addr, POOL_SPAN_PAGES);
    delete pool;
}

void CentralCache::releaseEmptyPoolsLocked(PoolTable& table, bool force)
{
    PoolInfo* current = force ? table.empty_pools : (table.empty_pools != nullptr ? table.empty_pools->next : nullptr);
    while (current != nullptr)
    {
        PoolInfo* next = current->next;
        releasePoolLocked(table, current);
        current = next;
    }
}

} // namespace Kama_memoryPool
