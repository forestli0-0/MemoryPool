#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"

#include <cstdlib>

namespace Kama_memoryPool
{

namespace
{

size_t countFreeList(void* start)
{
    size_t count = 0;
    void* current = start;
    while (current)
    {
        ++count;
        current = *reinterpret_cast<void**>(current);
    }

    return count;
}

} // namespace

void* ThreadCache::allocate(size_t size)
{
    if (size == 0)
    {
        size = ALIGNMENT;
    }

    if (size > MAX_BYTES)
    {
        return malloc(size);
    }

    size_t index = SizeClass::getIndex(size);
    if (void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        if (freeListSize_[index] > 0)
        {
            --freeListSize_[index];
        }
        return ptr;
    }

    return fetchFromCentralCache(index);
}

void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    ++freeListSize_[index];

    if (shouldReturnToCentralCache(index))
    {
        returnToCentralCache(freeList_[index], size);
    }
}

bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    size_t threshold = 64;
    return freeListSize_[index] > threshold;
}

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    size_t size = (index + 1) * ALIGNMENT;
    size_t batchNum = getBatchNum(size);
    void* start = CentralCache::getInstance().fetchRange(index, batchNum);
    if (!start)
    {
        return nullptr;
    }

    size_t fetchedCount = countFreeList(start);
    if (fetchedCount == 0)
    {
        return nullptr;
    }

    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);
    *reinterpret_cast<void**>(result) = nullptr;
    freeListSize_[index] += fetchedCount - 1;
    return result;
}

void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    size_t index = SizeClass::getIndex(size);
    size_t batchNum = countFreeList(start);
    if (batchNum <= 1)
    {
        return;
    }

    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t splitSteps = keepNum - 1;
    char* splitNode = static_cast<char*>(start);
    for (size_t i = 0; i < splitSteps && splitNode != nullptr; ++i)
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
    }

    if (splitNode == nullptr)
    {
        freeList_[index] = start;
        freeListSize_[index] = batchNum;
        return;
    }

    void* nextNode = *reinterpret_cast<void**>(splitNode);
    if (!nextNode)
    {
        freeList_[index] = start;
        freeListSize_[index] = keepNum;
        return;
    }

    *reinterpret_cast<void**>(splitNode) = nullptr;
    freeList_[index] = start;
    freeListSize_[index] = keepNum;
    CentralCache::getInstance().returnRange(nextNode, batchNum - keepNum, index);
}

size_t ThreadCache::getBatchNum(size_t size)
{
    constexpr size_t MAX_BATCH_SIZE = 4 * 1024;

    size_t baseNum;
    if (size <= 32) baseNum = 64;
    else if (size <= 64) baseNum = 32;
    else if (size <= 128) baseNum = 16;
    else if (size <= 256) baseNum = 8;
    else if (size <= 512) baseNum = 4;
    else if (size <= 1024) baseNum = 2;
    else baseNum = 1;

    size_t maxNum = std::max(size_t(1), MAX_BATCH_SIZE / size);
    return std::max(size_t(1), std::min(maxNum, baseNum));
}

} // namespace memoryPool
