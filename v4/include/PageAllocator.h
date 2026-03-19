#pragma once

#include "Common.h"

#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace glock
{

class PageAllocator
{
public:
    struct SpanInfo
    {
        void* addr = nullptr;
        size_t page_count = 0;
        bool is_free = false;
        bool is_small_pool_span = false;
        void* owner = nullptr;
        void* reservation_base = nullptr;
        size_t reservation_pages = 0;
    };

    static PageAllocator& getInstance()
    {
        static PageAllocator instance;
        return instance;
    }

    ~PageAllocator();

    void* allocateSpan(size_t pageCount, bool isSmallPoolSpan);
    void deallocateSpan(void* addr, size_t pageCount);
    void scavenge(bool force = false);
    void setSpanOwner(void* spanAddr, void* owner);

    size_t getCachedFreePages() const;
    size_t getReservedBytes() const;
    size_t getReleasedBytes() const;
    MemoryPoolStats::PageAllocatorRuntimeStats getRuntimeStats() const;

    template <typename Callback>
    void forEachSpanInBatch(FreeBlock* head, size_t count, Callback&& callback)
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        FreeBlock* current = head;
        size_t processed = 0;
        SpanInfo* lastSpan = nullptr;
        std::uintptr_t lastBegin = 0;
        std::uintptr_t lastEnd = 0;

        while (current != nullptr && processed < count)
        {
            FreeBlock* next = current->next;
            SpanInfo* span = nullptr;
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(current);
            if (lastSpan != nullptr && address >= lastBegin && address < lastEnd)
            {
                span = lastSpan;
            }
            else
            {
                span = findSpanContainingLocked(current);
                if (span != nullptr)
                {
                    lastSpan = span;
                    lastBegin = reinterpret_cast<std::uintptr_t>(span->addr);
                    lastEnd = lastBegin + span->page_count * PAGE_SIZE;
                }
                else
                {
                    lastSpan = nullptr;
                    lastBegin = 0;
                    lastEnd = 0;
                }
            }

            callback(current, span);
            current = next;
            ++processed;
        }
    }

private:
    PageAllocator() = default;
    PageAllocator(const PageAllocator&) = delete;
    PageAllocator& operator=(const PageAllocator&) = delete;

    SpanInfo* findSpanStartLocked(void* addr);
    SpanInfo* findSpanContainingLocked(void* addr);
    void mapSpanPagesLocked(SpanInfo* span);
    void unmapSpanPagesLocked(SpanInfo* span);
    void insertFreeSpanLocked(SpanInfo* span);
    void removeFreeSpanLocked(SpanInfo* span);
    void coalesceLocked(SpanInfo*& span);
    void releaseSpanLocked(SpanInfo* span);
    void scavengeLocked(bool force);
    size_t getImmediatelyFreeablePagesLocked() const;

    void* systemAlloc(size_t pageCount);
    void systemFree(void* addr, size_t pageCount);

private:
    std::map<std::uintptr_t, SpanInfo*> spansByAddr_;
    std::unordered_map<size_t, SpanInfo*> spansByPageId_;
    std::multimap<size_t, SpanInfo*> freeByPages_;
    mutable std::shared_mutex mutex_;
    std::atomic<size_t> cachedFreePages_{0};
    std::atomic<size_t> osReservedBytes_{0};
    std::atomic<size_t> osReleasedBytes_{0};
    std::atomic<size_t> spanAllocCalls_{0};
    std::atomic<size_t> spanFreeCalls_{0};
    std::atomic<size_t> cacheHitAllocs_{0};
    std::atomic<size_t> cacheMissAllocs_{0};
    std::atomic<size_t> systemAllocCalls_{0};
    std::atomic<size_t> systemFreeCalls_{0};
    std::atomic<size_t> coalesceMerges_{0};
    std::atomic<size_t> scavengeCalls_{0};
    std::atomic<size_t> releasedSpans_{0};
};

} // namespace glock
