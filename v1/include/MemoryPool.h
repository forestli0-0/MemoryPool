#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#if !defined(__SIZEOF_INT128__) || !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16) || INTPTR_MAX != INT64_MAX
#include <mutex>
#endif

namespace Kama_memoryPool
{

#define MEMORY_POOL_NUM 64
#define SLOT_BASE_SIZE 8
#define MAX_SLOT_SIZE 512

#if defined(__SIZEOF_INT128__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16) && INTPTR_MAX == INT64_MAX
#define KAMA_V1_LOCKFREE_ENABLED 1
#else
#define KAMA_V1_LOCKFREE_ENABLED 0
#endif

struct Slot
{
    std::atomic<Slot*> next;

    Slot()
        : next(nullptr)
    {
    }
};

class MemoryPool
{
public:
    explicit MemoryPool(size_t blockSize = 4096);
    ~MemoryPool();

    void init(size_t size);
    void* allocate();
    void deallocate(void* ptr);

private:
    size_t padPointer(char* ptr, size_t align) const;
    bool pushFreeList(Slot* slot);
    Slot* popFreeList();

#if KAMA_V1_LOCKFREE_ENABLED
    struct Block
    {
        std::atomic<Block*> next;
        std::atomic<size_t> nextSlotIndex;
        char* dataStart;
        size_t slotCount;

        Block()
            : next(nullptr)
            , nextSlotIndex(0)
            , dataStart(nullptr)
            , slotCount(0)
        {
        }
    };

    struct alignas(16) FreeListHead
    {
        Slot* ptr;
        std::uint64_t tag;
    };

    static_assert(std::is_trivially_copyable<FreeListHead>::value, "FreeListHead must be trivially copyable");
    static_assert(sizeof(FreeListHead) == 16, "FreeListHead must fit in 16 bytes");

    static __int128 packFreeListHead(const FreeListHead& head);
    static FreeListHead unpackFreeListHead(__int128 raw);

    FreeListHead loadFreeListHead() const;
    bool compareExchangeFreeListHead(FreeListHead& expected, const FreeListHead& desired);

    Block* createBlock();
    void publishBlock(Block* block);
    void* tryAllocateFromBlock(Block* block);
    void* tryAllocateFromBlocks();
#else
    void allocateNewBlock();
#endif

private:
    size_t BlockSize_;
    size_t SlotSize_;

#if KAMA_V1_LOCKFREE_ENABLED
    std::atomic<Block*> blockListHead_;
    alignas(16) volatile __int128 freeListHead_;
#else
    Slot* firstBlock_;
    Slot* curSlot_;
    std::atomic<Slot*> freeList_;
    Slot* lastSlot_;
    std::mutex mutexForFreeList_;
    std::mutex mutexForBlock_;
#endif
};

class HashBucket
{
public:
    static void initMemoryPool();
    static MemoryPool& getMemoryPool(int index);

    static void* useMemory(size_t size)
    {
        if (size == 0)
        {
            return nullptr;
        }

        if (size > MAX_SLOT_SIZE)
        {
            return operator new(size);
        }

        return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
    }

    static void freeMemory(void* ptr, size_t size)
    {
        if (!ptr)
        {
            return;
        }

        if (size > MAX_SLOT_SIZE)
        {
            operator delete(ptr);
            return;
        }

        getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
    }

    template<typename T, typename... Args>
    friend T* newElement(Args&&... args);

    template<typename T>
    friend void deleteElement(T* ptr);
};

template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    T* ptr = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)));
    if (ptr != nullptr)
    {
        new(ptr) T(std::forward<Args>(args)...);
    }

    return ptr;
}

template<typename T>
void deleteElement(T* ptr)
{
    if (ptr)
    {
        ptr->~T();
        HashBucket::freeMemory(reinterpret_cast<void*>(ptr), sizeof(T));
    }
}

} // namespace Kama_memoryPool
