#include "../include/CentralCache.h"
#include "../include/MemoryPool.h"
#include "../include/PageAllocator.h"
#include "../include/ThreadCache.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace glock;

namespace
{

void resetAllocator()
{
    MemoryPool::scavenge();
}

void testSizeClassMapping()
{
    std::cout << "Running size class mapping test..." << std::endl;
    assert(SizeClass::normalizeSize(0) == 8);
    assert(SizeClass::normalizeSize(1) == 8);
    assert(SizeClass::normalizeSize(8) == 8);
    assert(SizeClass::normalizeSize(9) == 16);
    assert(SizeClass::normalizeSize(24) == 24);
    assert(SizeClass::normalizeSize(25) == 32);
    assert(SizeClass::normalizeSize(513) == 640);
    assert(SizeClass::normalizeSize(1024) == 1024);
    assert(SizeClass::normalizeSize(1025) == 1025);
}

void testSmallAllocationAndWrite()
{
    std::cout << "Running small allocation test..." << std::endl;
    resetAllocator();

    const MemoryPoolStats before = MemoryPool::getStats();
    for (size_t size : SizeClass::kClassSizes)
    {
        char* ptr = static_cast<char*>(MemoryPool::allocate(size));
        assert(ptr != nullptr);
        std::memset(ptr, static_cast<int>(size & 0xFF), size);
        for (size_t index = 0; index < size; ++index)
        {
            assert(static_cast<unsigned char>(ptr[index]) == static_cast<unsigned char>(size & 0xFF));
        }
        MemoryPool::deallocate(ptr, size);
    }

    MemoryPool::scavenge();
    const MemoryPoolStats after = MemoryPool::getStats();
    assert(after.small_alloc_requests >= before.small_alloc_requests + SizeClass::kClassSizes.size());
    assert(after.active_pool_count == 0);
    assert(after.empty_pool_count == 0);
    assert(after.cached_free_pages == 0);
}

void testLargeAllocationPath()
{
    std::cout << "Running large allocation test..." << std::endl;
    resetAllocator();

    const MemoryPoolStats before = MemoryPool::getStats();
    std::vector<size_t> sizes = {1025, 64 * 1024, 1024 * 1024};
    std::vector<void*> pointers;

    for (size_t size : sizes)
    {
        char* ptr = static_cast<char*>(MemoryPool::allocate(size));
        assert(ptr != nullptr);
        ptr[0] = 0x2A;
        ptr[size - 1] = 0x5A;
        assert(ptr[0] == 0x2A);
        assert(ptr[size - 1] == 0x5A);
        pointers.push_back(ptr);
    }

    for (size_t index = 0; index < sizes.size(); ++index)
    {
        MemoryPool::deallocate(pointers[index], sizes[index]);
    }

    MemoryPool::scavenge();
    const MemoryPoolStats after = MemoryPool::getStats();
    assert(after.large_alloc_requests >= before.large_alloc_requests + sizes.size());
    assert(after.os_released_bytes >= before.os_released_bytes + (1025 + 64 * 1024 + 1024 * 1024) / 2);
}

void testPoolStateTransitions()
{
    std::cout << "Running pool state transition test..." << std::endl;
    resetAllocator();

    void* one = MemoryPool::allocate(1024);
    assert(one != nullptr);
    assert(CentralCache::getInstance().getActivePoolCount() >= 1);
    MemoryPool::deallocate(one, 1024);
    ThreadCache::getInstance()->flushAll();
    assert(CentralCache::getInstance().getEmptyPoolCount() >= 1);

    MemoryPool::scavenge();

    std::vector<void*> blocks;
    blocks.reserve(65);
    for (size_t index = 0; index < 65; ++index)
    {
        void* ptr = MemoryPool::allocate(1024);
        assert(ptr != nullptr);
        blocks.push_back(ptr);
    }

    assert(CentralCache::getInstance().getActivePoolCount() >= 2);

    MemoryPool::deallocate(blocks.front(), 1024);
    ThreadCache::getInstance()->flushAll();
    assert(CentralCache::getInstance().getActivePoolCount() >= 2);

    for (void* ptr : blocks)
    {
        if (ptr != blocks.front())
        {
            MemoryPool::deallocate(ptr, 1024);
        }
    }
    ThreadCache::getInstance()->flushAll();

    assert(CentralCache::getInstance().getActivePoolCount() == 0);
    assert(CentralCache::getInstance().getEmptyPoolCount() >= 1);

    MemoryPool::scavenge();
}

void testPageAllocatorReuseAndCoalesce()
{
    std::cout << "Running page allocator reuse test..." << std::endl;
    resetAllocator();

    PageAllocator& allocator = PageAllocator::getInstance();
    const size_t beforeReleased = allocator.getReleasedBytes();

    void* full = allocator.allocateSpan(POOL_SPAN_PAGES, false);
    assert(full != nullptr);
    allocator.deallocateSpan(full, POOL_SPAN_PAGES);

    void* left = allocator.allocateSpan(4, false);
    assert(left == full);
    void* right = allocator.allocateSpan(POOL_SPAN_PAGES - 4, false);
    assert(right == static_cast<char*>(full) + 4 * PAGE_SIZE);

    allocator.deallocateSpan(left, 4);
    allocator.deallocateSpan(right, POOL_SPAN_PAGES - 4);

    void* merged = allocator.allocateSpan(POOL_SPAN_PAGES, false);
    assert(merged == full);
    allocator.deallocateSpan(merged, POOL_SPAN_PAGES);
    allocator.scavenge(true);
    assert(allocator.getReleasedBytes() >= beforeReleased + POOL_SPAN_BYTES);
}

void testCrossThreadFree()
{
    std::cout << "Running cross-thread free test..." << std::endl;
    resetAllocator();

    std::vector<void*> allocations;
    std::thread producer([&allocations]()
    {
        allocations.reserve(128);
        for (size_t index = 0; index < 128; ++index)
        {
            allocations.push_back(MemoryPool::allocate(256));
        }
    });
    producer.join();

    std::thread consumer([&allocations]()
    {
        for (void* ptr : allocations)
        {
            MemoryPool::deallocate(ptr, 256);
        }
    });
    consumer.join();

    MemoryPool::scavenge();
    const MemoryPoolStats stats = MemoryPool::getStats();
    assert(stats.active_pool_count == 0);
    assert(stats.empty_pool_count == 0);
}

void testScavengeReleasesPages()
{
    std::cout << "Running scavenge release test..." << std::endl;
    resetAllocator();

    const MemoryPoolStats before = MemoryPool::getStats();
    std::vector<void*> allocations;
    allocations.reserve(256);
    for (size_t index = 0; index < 256; ++index)
    {
        allocations.push_back(MemoryPool::allocate(1024));
    }

    for (void* ptr : allocations)
    {
        MemoryPool::deallocate(ptr, 1024);
    }

    MemoryPool::scavenge();
    const MemoryPoolStats after = MemoryPool::getStats();
    assert(after.os_released_bytes > before.os_released_bytes);
    assert(after.cached_free_pages == 0);
}

void testRuntimeTuningRoundTrip()
{
    std::cout << "Running runtime tuning round-trip test..." << std::endl;
    resetAllocator();

    const MemoryPoolRuntimeTuning defaults = MemoryPool::getTuning();
    MemoryPoolRuntimeTuning tuned = defaults;
    tuned.batch_target_bytes = 12 * 1024;
    tuned.batch_max_count = 96;
    tuned.thread_cache_retain_batches = 4;
    tuned.thread_cache_high_water_batches = 8;
    tuned.central_empty_pools_small = 8;
    tuned.central_empty_pools_medium = 4;
    tuned.central_empty_pools_large = 2;
    tuned.page_release_high_water_pages = 2048;
    tuned.page_release_low_water_pages = 1024;
    tuned.page_auto_scavenge_trigger_pages = 8192;

    MemoryPool::applyTuning(tuned);
    const MemoryPoolRuntimeTuning applied = MemoryPool::getTuning();
    assert(applied.batch_target_bytes == tuned.batch_target_bytes);
    assert(applied.batch_max_count == tuned.batch_max_count);
    assert(applied.thread_cache_retain_batches == tuned.thread_cache_retain_batches);
    assert(applied.thread_cache_high_water_batches == tuned.thread_cache_high_water_batches);
    assert(applied.central_empty_pools_small == tuned.central_empty_pools_small);
    assert(applied.central_empty_pools_medium == tuned.central_empty_pools_medium);
    assert(applied.central_empty_pools_large == tuned.central_empty_pools_large);
    assert(applied.page_release_high_water_pages == tuned.page_release_high_water_pages);
    assert(applied.page_release_low_water_pages == tuned.page_release_low_water_pages);
    assert(applied.page_auto_scavenge_trigger_pages == tuned.page_auto_scavenge_trigger_pages);

    MemoryPool::resetTuning();
    const MemoryPoolRuntimeTuning reset = MemoryPool::getTuning();
    assert(reset.batch_target_bytes == defaults.batch_target_bytes);
    assert(reset.batch_max_count == defaults.batch_max_count);
    assert(reset.thread_cache_retain_batches == defaults.thread_cache_retain_batches);
    assert(reset.thread_cache_high_water_batches == defaults.thread_cache_high_water_batches);
    assert(reset.central_empty_pools_small == defaults.central_empty_pools_small);
    assert(reset.central_empty_pools_medium == defaults.central_empty_pools_medium);
    assert(reset.central_empty_pools_large == defaults.central_empty_pools_large);
    assert(reset.page_release_high_water_pages == defaults.page_release_high_water_pages);
    assert(reset.page_release_low_water_pages == defaults.page_release_low_water_pages);
    assert(reset.page_auto_scavenge_trigger_pages == defaults.page_auto_scavenge_trigger_pages);
}

} // namespace

int main()
{
    testSizeClassMapping();
    testSmallAllocationAndWrite();
    testLargeAllocationPath();
    testPoolStateTransitions();
    testPageAllocatorReuseAndCoalesce();
    testCrossThreadFree();
    testScavengeReleasesPages();
    testRuntimeTuningRoundTrip();

    std::cout << "All V4 unit tests passed." << std::endl;
    return 0;
}
