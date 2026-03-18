#include "../include/PageAllocator.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <unordered_map>
#include <utility>
#include <vector>

namespace Kama_memoryPool
{

namespace
{

std::uintptr_t toAddress(const void* ptr)
{
    return reinterpret_cast<std::uintptr_t>(ptr);
}

bool isWholeReservation(const PageAllocator::SpanInfo* span)
{
    return span != nullptr
        && span->addr == span->reservation_base
        && span->page_count == span->reservation_pages;
}

void* spanEnd(const PageAllocator::SpanInfo* span)
{
    return static_cast<char*>(span->addr) + span->page_count * PAGE_SIZE;
}

} // namespace

PageAllocator::~PageAllocator()
{
    std::vector<std::pair<void*, size_t>> reservations;
    std::vector<SpanInfo*> spans;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::unordered_map<std::uintptr_t, size_t> uniqueReservations;
        for (const auto& [addr, span] : spansByAddr_)
        {
            uniqueReservations[toAddress(span->reservation_base)] = span->reservation_pages;
            spans.push_back(span);
        }

        spansByAddr_.clear();
        freeByPages_.clear();
        cachedFreePages_.store(0, std::memory_order_relaxed);

        for (const auto& [base, pageCount] : uniqueReservations)
        {
            reservations.push_back({reinterpret_cast<void*>(base), pageCount});
        }
    }

    for (const auto& [base, pageCount] : reservations)
    {
        systemFree(base, pageCount);
    }

    for (SpanInfo* span : spans)
    {
        delete span;
    }
}

void* PageAllocator::allocateSpan(size_t pageCount, bool isSmallPoolSpan)
{
    if (pageCount == 0)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto freeIt = freeByPages_.lower_bound(pageCount);
    if (freeIt != freeByPages_.end())
    {
        SpanInfo* span = freeIt->second;
        removeFreeSpanLocked(span);

        if (span->page_count > pageCount)
        {
            SpanInfo* remainder = new SpanInfo;
            remainder->addr = static_cast<char*>(span->addr) + pageCount * PAGE_SIZE;
            remainder->page_count = span->page_count - pageCount;
            remainder->is_free = true;
            remainder->is_small_pool_span = false;
            remainder->owner = nullptr;
            remainder->reservation_base = span->reservation_base;
            remainder->reservation_pages = span->reservation_pages;
            spansByAddr_[toAddress(remainder->addr)] = remainder;
            insertFreeSpanLocked(remainder);
            span->page_count = pageCount;
        }

        span->is_free = false;
        span->is_small_pool_span = isSmallPoolSpan;
        span->owner = nullptr;
        return span->addr;
    }

    void* memory = systemAlloc(pageCount);
    if (!memory)
    {
        return nullptr;
    }

    SpanInfo* span = new SpanInfo;
    span->addr = memory;
    span->page_count = pageCount;
    span->is_free = false;
    span->is_small_pool_span = isSmallPoolSpan;
    span->owner = nullptr;
    span->reservation_base = memory;
    span->reservation_pages = pageCount;
    spansByAddr_[toAddress(memory)] = span;
    osReservedBytes_.fetch_add(pageCount * PAGE_SIZE, std::memory_order_relaxed);
    return memory;
}

void PageAllocator::deallocateSpan(void* addr, size_t pageCount)
{
    if (!addr || pageCount == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    SpanInfo* span = findSpanStartLocked(addr);
    if (span == nullptr || span->is_free)
    {
        return;
    }

    span->is_free = true;
    span->is_small_pool_span = false;
    span->owner = nullptr;
    coalesceLocked(span);
    insertFreeSpanLocked(span);
    scavengeLocked(false);
}

void PageAllocator::scavenge(bool force)
{
    std::lock_guard<std::mutex> lock(mutex_);
    scavengeLocked(force);
}

size_t PageAllocator::getCachedFreePages() const
{
    return cachedFreePages_.load(std::memory_order_relaxed);
}

size_t PageAllocator::getReservedBytes() const
{
    return osReservedBytes_.load(std::memory_order_relaxed);
}

size_t PageAllocator::getReleasedBytes() const
{
    return osReleasedBytes_.load(std::memory_order_relaxed);
}

void PageAllocator::setSpanOwner(void* spanAddr, void* owner)
{
    if (spanAddr == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    SpanInfo* span = findSpanStartLocked(spanAddr);
    if (span != nullptr)
    {
        span->owner = owner;
    }
}

PageAllocator::SpanInfo* PageAllocator::findSpanStartLocked(void* addr)
{
    auto spanIt = spansByAddr_.find(toAddress(addr));
    if (spanIt == spansByAddr_.end())
    {
        return nullptr;
    }

    return spanIt->second;
}

PageAllocator::SpanInfo* PageAllocator::findSpanContainingLocked(void* addr)
{
    if (addr == nullptr || spansByAddr_.empty())
    {
        return nullptr;
    }

    auto it = spansByAddr_.upper_bound(toAddress(addr));
    if (it == spansByAddr_.begin())
    {
        return nullptr;
    }

    --it;
    SpanInfo* span = it->second;
    const std::uintptr_t begin = toAddress(span->addr);
    const std::uintptr_t end = begin + span->page_count * PAGE_SIZE;
    const std::uintptr_t address = toAddress(addr);
    if (address < begin || address >= end)
    {
        return nullptr;
    }

    return span;
}

void PageAllocator::insertFreeSpanLocked(SpanInfo* span)
{
    span->is_free = true;
    freeByPages_.insert({span->page_count, span});
    cachedFreePages_.fetch_add(span->page_count, std::memory_order_relaxed);
}

void PageAllocator::removeFreeSpanLocked(SpanInfo* span)
{
    auto [begin, end] = freeByPages_.equal_range(span->page_count);
    for (auto it = begin; it != end; ++it)
    {
        if (it->second == span)
        {
            freeByPages_.erase(it);
            cachedFreePages_.fetch_sub(span->page_count, std::memory_order_relaxed);
            return;
        }
    }
}

void PageAllocator::coalesceLocked(SpanInfo*& span)
{
    bool merged = true;
    while (merged)
    {
        merged = false;
        auto currentIt = spansByAddr_.find(toAddress(span->addr));
        if (currentIt == spansByAddr_.end())
        {
            break;
        }

        if (currentIt != spansByAddr_.begin())
        {
            auto prevIt = std::prev(currentIt);
            SpanInfo* prev = prevIt->second;
            if (prev->is_free
                && prev->reservation_base == span->reservation_base
                && spanEnd(prev) == span->addr)
            {
                removeFreeSpanLocked(prev);
                prev->page_count += span->page_count;
                spansByAddr_.erase(currentIt);
                delete span;
                span = prev;
                merged = true;
                continue;
            }
        }

        currentIt = spansByAddr_.find(toAddress(span->addr));
        auto nextIt = std::next(currentIt);
        if (nextIt != spansByAddr_.end())
        {
            SpanInfo* next = nextIt->second;
            if (next->is_free
                && next->reservation_base == span->reservation_base
                && spanEnd(span) == next->addr)
            {
                removeFreeSpanLocked(next);
                span->page_count += next->page_count;
                spansByAddr_.erase(nextIt);
                delete next;
                merged = true;
            }
        }
    }
}

void PageAllocator::releaseSpanLocked(SpanInfo* span)
{
    if (!isWholeReservation(span))
    {
        return;
    }

    removeFreeSpanLocked(span);
    spansByAddr_.erase(toAddress(span->addr));
    systemFree(span->addr, span->page_count);
    osReleasedBytes_.fetch_add(span->page_count * PAGE_SIZE, std::memory_order_relaxed);
    delete span;
}

void PageAllocator::scavengeLocked(bool force)
{
    const size_t cachedPages = cachedFreePages_.load(std::memory_order_relaxed);
    if (!force && cachedPages <= RELEASE_HIGH_WATER_PAGES)
    {
        return;
    }

    std::vector<SpanInfo*> releasable;
    size_t remainingPages = cachedPages;
    for (auto it = freeByPages_.rbegin(); it != freeByPages_.rend(); ++it)
    {
        SpanInfo* span = it->second;
        if (!isWholeReservation(span))
        {
            continue;
        }

        releasable.push_back(span);
        if (!force)
        {
            remainingPages = (remainingPages > span->page_count) ? (remainingPages - span->page_count) : 0;
            if (remainingPages <= RELEASE_LOW_WATER_PAGES)
            {
                break;
            }
        }
    }

    for (SpanInfo* span : releasable)
    {
        if (!force && cachedFreePages_.load(std::memory_order_relaxed) <= RELEASE_LOW_WATER_PAGES)
        {
            break;
        }

        auto spanIt = spansByAddr_.find(toAddress(span->addr));
        if (spanIt == spansByAddr_.end())
        {
            continue;
        }

        SpanInfo* current = spanIt->second;
        if (current->is_free && isWholeReservation(current))
        {
            releaseSpanLocked(current);
        }
    }
}

void* PageAllocator::systemAlloc(size_t pageCount)
{
    const size_t bytes = pageCount * PAGE_SIZE;
#ifdef _WIN32
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr;
#endif
}

void PageAllocator::systemFree(void* addr, size_t pageCount)
{
    if (!addr || pageCount == 0)
    {
        return;
    }

#ifdef _WIN32
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    munmap(addr, pageCount * PAGE_SIZE);
#endif
}

} // namespace Kama_memoryPool
