#include "UnifiedBenchmark.h"
#include "MemoryPool.h"

#include <mutex>

namespace
{

struct V1Allocator
{
    static const char* label()
    {
        return "V1";
    }

    static void initialize()
    {
        static std::once_flag once;
        std::call_once(once, []()
        {
            Kama_memoryPool::HashBucket::initMemoryPool();
        });
    }

    static void* allocate(size_t size)
    {
        return Kama_memoryPool::HashBucket::useMemory(size);
    }

    static void deallocate(void* ptr, size_t size)
    {
        Kama_memoryPool::HashBucket::freeMemory(ptr, size);
    }

    static void cleanup()
    {
    }
};

} // namespace

int main()
{
    return unified_bench::runMain<V1Allocator>();
}
