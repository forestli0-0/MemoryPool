#pragma once

#include "Common.h"

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace Kama_memoryPool
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

    template <typename Callback>
    void forEachSpanInBatch(FreeBlock* head, size_t count, Callback&& callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);

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
    void insertFreeSpanLocked(SpanInfo* span);
    void removeFreeSpanLocked(SpanInfo* span);
    void coalesceLocked(SpanInfo*& span);
    void releaseSpanLocked(SpanInfo* span);
    void scavengeLocked(bool force);

    void* systemAlloc(size_t pageCount);
    void systemFree(void* addr, size_t pageCount);

private:
    std::map<std::uintptr_t, SpanInfo*> spansByAddr_;
    std::multimap<size_t, SpanInfo*> freeByPages_;
    mutable std::mutex mutex_;
    std::atomic<size_t> cachedFreePages_{0};
    std::atomic<size_t> osReservedBytes_{0};
    std::atomic<size_t> osReleasedBytes_{0};
};

} // namespace Kama_memoryPool
