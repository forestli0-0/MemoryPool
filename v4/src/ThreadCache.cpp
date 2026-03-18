#include "../include/ThreadCache.h"

#include "../include/CentralCache.h"
#include "../include/PageAllocator.h"

namespace Kama_memoryPool
{

ThreadCache::ThreadCache()
{
    freeLists_.fill(nullptr);
    counts_.fill(0);
}

ThreadCache::~ThreadCache()
{
    flushAll();
}

void* ThreadCache::allocate(size_t size)
{
    const size_t normalized = (size == 0) ? ALIGNMENT : size;
    if (normalized > SMALL_ALLOC_MAX)
    {
        return PageAllocator::getInstance().allocateSpan(roundUpToPages(normalized), false);
    }

    const size_t classIndex = SizeClass::getClassIndex(normalized);
    if (freeLists_[classIndex] == nullptr)
    {
        fetchBatch(classIndex);
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
        PageAllocator::getInstance().deallocateSpan(ptr, roundUpToPages(normalized));
        return;
    }

    const size_t classIndex = SizeClass::getClassIndex(normalized);
    pushLocal(classIndex, static_cast<FreeBlock*>(ptr));

    const size_t batchCount = SizeClass::getBatchCount(SizeClass::getClassSize(classIndex));
    if (counts_[classIndex] > batchCount * 2)
    {
        returnBatch(classIndex, batchCount);
    }
}

void ThreadCache::flushAll()
{
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
        CentralCache::getInstance().releaseBatch(classIndex, head, count);
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
    CentralCache::getInstance().releaseBatch(classIndex, head, actualCount);
}

} // namespace Kama_memoryPool
