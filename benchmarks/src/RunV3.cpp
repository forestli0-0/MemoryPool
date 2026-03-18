#include "UnifiedBenchmark.h"
#include "MemoryPool.h"

namespace
{

struct V3Allocator
{
    static const char* label()
    {
        return "V3";
    }

    static void initialize()
    {
    }

    static void* allocate(size_t size)
    {
        return Kama_memoryPool::MemoryPool::allocate(size);
    }

    static void deallocate(void* ptr, size_t size)
    {
        Kama_memoryPool::MemoryPool::deallocate(ptr, size);
    }

    static void cleanup()
    {
    }
};

} // namespace

int main()
{
    return unified_bench::runMain<V3Allocator>();
}
