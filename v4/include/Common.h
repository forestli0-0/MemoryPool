#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Kama_memoryPool
{

constexpr size_t ALIGNMENT = 8;
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t POOL_SPAN_PAGES = 16;
constexpr size_t POOL_SPAN_BYTES = PAGE_SIZE * POOL_SPAN_PAGES;
constexpr size_t SMALL_ALLOC_MAX = 1024;
constexpr size_t RELEASE_HIGH_WATER_PAGES = 1024;
constexpr size_t RELEASE_LOW_WATER_PAGES = 512;
constexpr size_t SIZE_CLASS_COUNT = 24;

struct FreeBlock
{
    FreeBlock* next;
};

enum class PoolState
{
    Partial,
    Full,
    Empty
};

struct MemoryPoolStats
{
    size_t small_alloc_requests = 0;
    size_t large_alloc_requests = 0;
    size_t active_pool_count = 0;
    size_t empty_pool_count = 0;
    size_t cached_free_pages = 0;
    size_t os_reserved_bytes = 0;
    size_t os_released_bytes = 0;
};

class SizeClass
{
public:
    inline static constexpr std::array<size_t, SIZE_CLASS_COUNT> kClassSizes = {
        8, 16, 24, 32, 40, 48, 56, 64,
        80, 96, 112, 128,
        160, 192, 224, 256,
        320, 384, 448, 512,
        640, 768, 896, 1024
    };

    static size_t getClassIndex(size_t bytes)
    {
        const size_t normalized = std::max(bytes, ALIGNMENT);
        const auto it = std::lower_bound(kClassSizes.begin(), kClassSizes.end(), normalized);
        return static_cast<size_t>(it - kClassSizes.begin());
    }

    static size_t getClassSize(size_t index)
    {
        return kClassSizes[index];
    }

    static size_t normalizeSize(size_t bytes)
    {
        if (bytes == 0)
        {
            bytes = ALIGNMENT;
        }

        if (bytes > SMALL_ALLOC_MAX)
        {
            return bytes;
        }

        return getClassSize(getClassIndex(bytes));
    }

    static size_t getBatchCount(size_t classSize)
    {
        const size_t maxBatch = std::max<size_t>(1, 4096 / classSize);
        return std::clamp(maxBatch, size_t(1), size_t(32));
    }
};

inline size_t toPageId(const void* ptr)
{
    return static_cast<size_t>(reinterpret_cast<std::uintptr_t>(ptr) / PAGE_SIZE);
}

inline size_t roundUpToPages(size_t bytes)
{
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

} // namespace Kama_memoryPool
