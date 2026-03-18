cmake_minimum_required(VERSION 3.10)
project(memory_pool_unified_benchmarks)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -O2)

find_package(Threads REQUIRED)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(BENCH_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(unified_v1_bench
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunV1.cpp
    ${REPO_ROOT}/v1/src/MemoryPool.cpp
)
target_include_directories(unified_v1_bench PRIVATE
    ${BENCH_INCLUDE}
    ${REPO_ROOT}/v1/include
)
target_link_libraries(unified_v1_bench PRIVATE Threads::Threads)

add_executable(unified_v3_bench
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunV3.cpp
    ${REPO_ROOT}/v3/src/CentralCache.cpp
    ${REPO_ROOT}/v3/src/PageCache.cpp
    ${REPO_ROOT}/v3/src/ThreadCache.cpp
)
target_include_directories(unified_v3_bench PRIVATE
    ${BENCH_INCLUDE}
    ${REPO_ROOT}/v3/include
)
target_link_libraries(unified_v3_bench PRIVATE Threads::Threads)

add_executable(unified_v4_bench
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunV4.cpp
    ${REPO_ROOT}/v4/src/CentralCache.cpp
    ${REPO_ROOT}/v4/src/PageAllocator.cpp
    ${REPO_ROOT}/v4/src/ThreadCache.cpp
)
target_include_directories(unified_v4_bench PRIVATE
    ${BENCH_INCLUDE}
    ${REPO_ROOT}/v4/include
)
target_link_libraries(unified_v4_bench PRIVATE Threads::Threads)
