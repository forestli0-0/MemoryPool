#pragma once

#include "Common.h"
#include <mutex>
#include <unordered_map>

namespace Kama_memoryPool
{

class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index, size_t batchNum);
    void returnRange(void* start, size_t count, size_t index);

private:
    struct SpanTracker
    {
        void* spanAddr = nullptr;
        size_t numPages = 0;
        size_t blockCount = 0;
        size_t freeCount = 0;
        size_t index = 0;
    };

    CentralCache()
    {
        for (auto& ptr : centralFreeList_)
        {
            ptr.store(nullptr, std::memory_order_relaxed);
        }

        for (auto& lock : locks_)
        {
            lock.clear();
        }
    }

    void* fetchFromPageCache(size_t size);
    SpanTracker* getSpanTrackerUnlocked(void* blockAddr);
    void trackSpan(void* spanAddr, size_t numPages, size_t blockCount, size_t freeCount, size_t index);
    void updateFreeCountForList(void* start, bool increase);
    void releaseFullyFreeSpans(size_t index);

private:
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
    std::unordered_map<void*, SpanTracker> spanTrackers_;
    std::mutex spanMutex_;
};

} // namespace memoryPool
