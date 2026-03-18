#include "../include/CentralCache.h"
#include "../include/PageCache.h"

#include <cassert>
#include <thread>
#include <vector>

namespace Kama_memoryPool
{

namespace
{

static const size_t SPAN_PAGES = 8;

size_t getSpanPages(size_t size)
{
    const size_t fixedSpanBytes = SPAN_PAGES * PageCache::PAGE_SIZE;
    if (size <= fixedSpanBytes)
    {
        return SPAN_PAGES;
    }

    return (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
}

} // namespace

void* CentralCache::fetchRange(size_t index, size_t batchNum)
{
    if (index >= FREE_LIST_SIZE || batchNum == 0)
    {
        return nullptr;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    void* result = nullptr;
    try
    {
        result = centralFreeList_[index].load(std::memory_order_relaxed);
        if (!result)
        {
            const size_t size = (index + 1) * ALIGNMENT;
            const size_t numPages = getSpanPages(size);
            result = fetchFromPageCache(size);
            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            char* start = static_cast<char*>(result);
            const size_t totalBlocks = (numPages * PageCache::PAGE_SIZE) / size;
            const size_t allocBlocks = std::min(batchNum, totalBlocks);
            assert(allocBlocks > 0);

            for (size_t i = 1; i < allocBlocks; ++i)
            {
                void* current = start + (i - 1) * size;
                void* next = start + i * size;
                *reinterpret_cast<void**>(current) = next;
            }
            *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;

            if (totalBlocks > allocBlocks)
            {
                void* remainStart = start + allocBlocks * size;
                for (size_t i = allocBlocks + 1; i < totalBlocks; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (totalBlocks - 1) * size) = nullptr;
                centralFreeList_[index].store(remainStart, std::memory_order_release);
            }
            else
            {
                centralFreeList_[index].store(nullptr, std::memory_order_release);
            }

            trackSpan(result, numPages, totalBlocks, totalBlocks - allocBlocks, index);
        }
        else
        {
            void* current = result;
            void* prev = nullptr;
            size_t count = 0;
            while (current && count < batchNum)
            {
                prev = current;
                current = *reinterpret_cast<void**>(current);
                ++count;
            }

            if (prev)
            {
                *reinterpret_cast<void**>(prev) = nullptr;
            }

            centralFreeList_[index].store(current, std::memory_order_release);
            updateFreeCountForList(result, false);
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
    return result;
}

void CentralCache::returnRange(void* start, size_t count, size_t index)
{
    if (!start || count == 0 || index >= FREE_LIST_SIZE)
    {
        return;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        void* end = start;
        size_t actualCount = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && actualCount < count)
        {
            end = *reinterpret_cast<void**>(end);
            ++actualCount;
        }

        updateFreeCountForList(start, true);

        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = current;
        centralFreeList_[index].store(start, std::memory_order_release);
        releaseFullyFreeSpans(index);
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

void* CentralCache::fetchFromPageCache(size_t size)
{
    return PageCache::getInstance().allocateSpan(getSpanPages(size));
}

CentralCache::SpanTracker* CentralCache::getSpanTrackerUnlocked(void* blockAddr)
{
    char* block = static_cast<char*>(blockAddr);
    for (auto& [spanAddr, tracker] : spanTrackers_)
    {
        char* spanStart = static_cast<char*>(spanAddr);
        char* spanEnd = spanStart + tracker.numPages * PageCache::PAGE_SIZE;
        if (block >= spanStart && block < spanEnd)
        {
            return &tracker;
        }
    }

    return nullptr;
}

void CentralCache::trackSpan(void* spanAddr, size_t numPages, size_t blockCount, size_t freeCount, size_t index)
{
    std::lock_guard<std::mutex> lock(spanMutex_);
    spanTrackers_[spanAddr] = SpanTracker{spanAddr, numPages, blockCount, freeCount, index};
}

void CentralCache::updateFreeCountForList(void* start, bool increase)
{
    std::lock_guard<std::mutex> lock(spanMutex_);
    std::unordered_map<void*, size_t> updates;

    void* current = start;
    while (current)
    {
        SpanTracker* tracker = getSpanTrackerUnlocked(current);
        if (tracker)
        {
            updates[tracker->spanAddr]++;
        }
        current = *reinterpret_cast<void**>(current);
    }

    for (const auto& [spanAddr, count] : updates)
    {
        auto it = spanTrackers_.find(spanAddr);
        if (it == spanTrackers_.end())
        {
            continue;
        }

        if (increase)
        {
            it->second.freeCount += count;
        }
        else
        {
            it->second.freeCount = (it->second.freeCount > count) ? (it->second.freeCount - count) : 0;
        }
    }
}

void CentralCache::releaseFullyFreeSpans(size_t index)
{
    std::vector<SpanTracker> reclaimable;
    {
        std::lock_guard<std::mutex> lock(spanMutex_);
        for (const auto& [spanAddr, tracker] : spanTrackers_)
        {
            if (tracker.index == index && tracker.blockCount > 0 && tracker.freeCount == tracker.blockCount)
            {
                reclaimable.push_back(tracker);
            }
        }
    }

    for (const SpanTracker& tracker : reclaimable)
    {
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* prev = nullptr;
        void* current = head;
        char* spanStart = static_cast<char*>(tracker.spanAddr);
        char* spanEnd = spanStart + tracker.numPages * PageCache::PAGE_SIZE;

        while (current)
        {
            void* next = *reinterpret_cast<void**>(current);
            char* block = static_cast<char*>(current);
            if (block >= spanStart && block < spanEnd)
            {
                if (prev)
                {
                    *reinterpret_cast<void**>(prev) = next;
                }
                else
                {
                    head = next;
                }
            }
            else
            {
                prev = current;
            }
            current = next;
        }

        centralFreeList_[index].store(head, std::memory_order_release);
        PageCache::getInstance().deallocateSpan(tracker.spanAddr, tracker.numPages);

        std::lock_guard<std::mutex> lock(spanMutex_);
        spanTrackers_.erase(tracker.spanAddr);
    }
}

} // namespace memoryPool
