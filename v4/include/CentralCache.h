#pragma once

#include "Common.h"

#include <array>
#include <atomic>
#include <mutex>

namespace glock
{

class CentralCache
{
public:
    struct PoolInfo
    {
        void* span_addr = nullptr;
        FreeBlock* free_list = nullptr;
        uint32_t capacity = 0;
        uint32_t free_count = 0;
        uint16_t size_class_index = 0;
        PoolState state = PoolState::Empty;
        PoolInfo* prev = nullptr;
        PoolInfo* next = nullptr;
    };

    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    FreeBlock* acquireBatch(size_t classIndex, size_t requestedCount, size_t& actualCount);
    void releaseBatch(size_t classIndex, FreeBlock* head, size_t count);
    void scavenge(bool force);

    size_t getActivePoolCount() const;
    size_t getEmptyPoolCount() const;
    MemoryPoolStats::CentralCacheRuntimeStats getRuntimeStats() const;

private:
    struct PoolTable
    {
        std::mutex lock;
        PoolInfo* partial_pools = nullptr;
        PoolInfo* full_pools = nullptr;
        PoolInfo* empty_pools = nullptr;
        size_t size_class_bytes = 0;
        size_t empty_pool_count = 0;
    };

    CentralCache();
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    PoolInfo* createPoolLocked(size_t classIndex, PoolTable& table);
    FreeBlock* popBatchFromPoolLocked(PoolTable& table, PoolInfo* pool, size_t requestedCount, size_t& actualCount);
    void pushBlockToPoolLocked(PoolTable& table, PoolInfo* pool, FreeBlock* block);
    void setPoolStateLocked(PoolTable& table, PoolInfo* pool, PoolState newState);
    void addPoolToList(PoolInfo*& head, PoolInfo* pool);
    void removePoolFromList(PoolInfo*& head, PoolInfo* pool);
    void releasePoolLocked(PoolTable& table, PoolInfo* pool);
    size_t getRetainedEmptyPoolLimit(const PoolTable& table) const;
    void releaseEmptyPoolsLocked(PoolTable& table, bool force);

private:
    std::array<PoolTable, SIZE_CLASS_COUNT> tables_;
    std::atomic<size_t> activePoolCount_{0};
    std::atomic<size_t> emptyPoolCount_{0};
    std::atomic<size_t> acquireCalls_{0};
    std::atomic<size_t> releaseCalls_{0};
    std::atomic<size_t> blocksAcquired_{0};
    std::atomic<size_t> blocksReleased_{0};
    std::atomic<size_t> partialPoolHits_{0};
    std::atomic<size_t> emptyPoolHits_{0};
    std::atomic<size_t> poolsCreated_{0};
    std::atomic<size_t> poolsReleased_{0};
    std::atomic<size_t> scavengeCalls_{0};
};

} // namespace glock
