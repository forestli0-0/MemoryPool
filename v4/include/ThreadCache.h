#pragma once

#include "Common.h"

#include <array>

namespace Kama_memoryPool
{

class ThreadCache
{
public:
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance;
        return &instance;
    }

    ~ThreadCache();

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);
    void flushAll();

private:
    ThreadCache();
    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;

    FreeBlock* popLocal(size_t classIndex);
    void pushLocal(size_t classIndex, FreeBlock* block);
    void fetchBatch(size_t classIndex);
    void returnBatch(size_t classIndex, size_t batchCount);

private:
    std::array<FreeBlock*, SIZE_CLASS_COUNT> freeLists_;
    std::array<size_t, SIZE_CLASS_COUNT> counts_;
};

} // namespace Kama_memoryPool
