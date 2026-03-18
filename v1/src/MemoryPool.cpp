#include "../include/MemoryPool.h"

#include <cstring>

namespace Kama_memoryPool
{

MemoryPool::MemoryPool(size_t blockSize)
    : BlockSize_(blockSize)
    , SlotSize_(0)
#if KAMA_V1_LOCKFREE_ENABLED
    , blockListHead_(nullptr)
    , freeListHead_(0)
#else
    , firstBlock_(nullptr)
    , curSlot_(nullptr)
    , freeList_(nullptr)
    , lastSlot_(nullptr)
#endif
{
}

MemoryPool::~MemoryPool()
{
#if KAMA_V1_LOCKFREE_ENABLED
    Block* current = blockListHead_.load(std::memory_order_relaxed);
    while (current != nullptr)
    {
        Block* next = current->next.load(std::memory_order_relaxed);
        current->~Block();
        operator delete(static_cast<void*>(current));
        current = next;
    }
#else
    Slot* cur = firstBlock_;
    while (cur != nullptr)
    {
        Slot* next = cur->next.load(std::memory_order_relaxed);
        operator delete(reinterpret_cast<void*>(cur));
        cur = next;
    }
#endif
}

void MemoryPool::init(size_t size)
{
    assert(size > 0);
    SlotSize_ = size;

#if KAMA_V1_LOCKFREE_ENABLED
    blockListHead_.store(nullptr, std::memory_order_relaxed);
    freeListHead_ = packFreeListHead({nullptr, 0});
#else
    firstBlock_ = nullptr;
    curSlot_ = nullptr;
    freeList_.store(nullptr, std::memory_order_relaxed);
    lastSlot_ = nullptr;
#endif
}

void* MemoryPool::allocate()
{
    if (Slot* slot = popFreeList())
    {
        return slot;
    }

#if KAMA_V1_LOCKFREE_ENABLED
    if (void* slot = tryAllocateFromBlocks())
    {
        return slot;
    }

    Block* block = createBlock();
    publishBlock(block);
    return block->dataStart;
#else
    Slot* temp = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutexForBlock_);
        if (curSlot_ >= lastSlot_)
        {
            allocateNewBlock();
        }

        temp = curSlot_;
        curSlot_ += SlotSize_ / sizeof(Slot);
    }

    return temp;
#endif
}

void MemoryPool::deallocate(void* ptr)
{
    if (!ptr)
    {
        return;
    }

    Slot* slot = reinterpret_cast<Slot*>(ptr);
    pushFreeList(slot);
}

size_t MemoryPool::padPointer(char* ptr, size_t align) const
{
    const size_t misalignment = reinterpret_cast<size_t>(ptr) % align;
    return (align - misalignment) % align;
}

bool MemoryPool::pushFreeList(Slot* slot)
{
#if KAMA_V1_LOCKFREE_ENABLED
    new(static_cast<void*>(slot)) Slot();

    FreeListHead expected = loadFreeListHead();
    FreeListHead desired{};
    do
    {
        slot->next.store(expected.ptr, std::memory_order_relaxed);
        desired.ptr = slot;
        desired.tag = expected.tag + 1;
    }
    while (!compareExchangeFreeListHead(expected, desired));

    return true;
#else
    std::lock_guard<std::mutex> lock(mutexForFreeList_);
    slot->next.store(freeList_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    freeList_.store(slot, std::memory_order_relaxed);
    return true;
#endif
}

Slot* MemoryPool::popFreeList()
{
#if KAMA_V1_LOCKFREE_ENABLED
    FreeListHead expected = loadFreeListHead();
    while (expected.ptr != nullptr)
    {
        Slot* next = expected.ptr->next.load(std::memory_order_acquire);
        FreeListHead desired{next, expected.tag + 1};
        if (compareExchangeFreeListHead(expected, desired))
        {
            return expected.ptr;
        }
    }

    return nullptr;
#else
    std::lock_guard<std::mutex> lock(mutexForFreeList_);
    Slot* head = freeList_.load(std::memory_order_relaxed);
    if (head)
    {
        freeList_.store(head->next.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return head;
#endif
}

#if KAMA_V1_LOCKFREE_ENABLED

__int128 MemoryPool::packFreeListHead(const FreeListHead& head)
{
    __int128 raw = 0;
    std::memcpy(&raw, &head, sizeof(head));
    return raw;
}

MemoryPool::FreeListHead MemoryPool::unpackFreeListHead(__int128 raw)
{
    FreeListHead head{};
    std::memcpy(&head, &raw, sizeof(head));
    return head;
}

MemoryPool::FreeListHead MemoryPool::loadFreeListHead() const
{
    const __int128 raw = __sync_val_compare_and_swap(
        const_cast<volatile __int128*>(&freeListHead_),
        static_cast<__int128>(0),
        static_cast<__int128>(0));

    return unpackFreeListHead(raw);
}

bool MemoryPool::compareExchangeFreeListHead(FreeListHead& expected, const FreeListHead& desired)
{
    const __int128 expectedRaw = packFreeListHead(expected);
    const __int128 desiredRaw = packFreeListHead(desired);
    const __int128 actualRaw = __sync_val_compare_and_swap(&freeListHead_, expectedRaw, desiredRaw);

    if (actualRaw == expectedRaw)
    {
        return true;
    }

    expected = unpackFreeListHead(actualRaw);
    return false;
}

MemoryPool::Block* MemoryPool::createBlock()
{
    void* raw = operator new(BlockSize_);
    Block* block = new(raw) Block();

    char* body = reinterpret_cast<char*>(raw) + sizeof(Block);
    const size_t paddingSize = padPointer(body, SlotSize_);
    char* dataStart = body + paddingSize;
    const size_t usableBytes = BlockSize_ - static_cast<size_t>(dataStart - reinterpret_cast<char*>(raw));
    const size_t slotCount = usableBytes / SlotSize_;

    assert(slotCount > 0);

    block->dataStart = dataStart;
    block->slotCount = slotCount;
    block->nextSlotIndex.store(1, std::memory_order_relaxed);
    block->next.store(nullptr, std::memory_order_relaxed);
    return block;
}

void MemoryPool::publishBlock(Block* block)
{
    Block* expected = blockListHead_.load(std::memory_order_acquire);
    do
    {
        block->next.store(expected, std::memory_order_relaxed);
    }
    while (!blockListHead_.compare_exchange_weak(
        expected,
        block,
        std::memory_order_release,
        std::memory_order_acquire));
}

void* MemoryPool::tryAllocateFromBlock(Block* block)
{
    size_t slotIndex = block->nextSlotIndex.load(std::memory_order_acquire);
    while (slotIndex < block->slotCount)
    {
        if (block->nextSlotIndex.compare_exchange_weak(
            slotIndex,
            slotIndex + 1,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
        {
            return block->dataStart + slotIndex * SlotSize_;
        }
    }

    return nullptr;
}

void* MemoryPool::tryAllocateFromBlocks()
{
    Block* current = blockListHead_.load(std::memory_order_acquire);
    while (current != nullptr)
    {
        if (void* slot = tryAllocateFromBlock(current))
        {
            return slot;
        }

        current = current->next.load(std::memory_order_acquire);
    }

    return nullptr;
}

#else

void MemoryPool::allocateNewBlock()
{
    void* newBlock = operator new(BlockSize_);
    reinterpret_cast<Slot*>(newBlock)->next.store(firstBlock_, std::memory_order_relaxed);
    firstBlock_ = reinterpret_cast<Slot*>(newBlock);

    char* body = reinterpret_cast<char*>(newBlock) + sizeof(Slot*);
    size_t paddingSize = padPointer(body, SlotSize_);
    curSlot_ = reinterpret_cast<Slot*>(body + paddingSize);
    lastSlot_ = reinterpret_cast<Slot*>(reinterpret_cast<size_t>(newBlock) + BlockSize_ - SlotSize_ + 1);
}

#endif

void HashBucket::initMemoryPool()
{
    for (int i = 0; i < MEMORY_POOL_NUM; ++i)
    {
        getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE);
    }
}

MemoryPool& HashBucket::getMemoryPool(int index)
{
    static MemoryPool memoryPool[MEMORY_POOL_NUM];
    return memoryPool[index];
}

} // namespace Kama_memoryPool
