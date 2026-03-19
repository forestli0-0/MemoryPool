cmake_minimum_required(VERSION 3.10)
project(memory_pool_v4)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -O2)

find_package(Threads REQUIRED)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)
set(TEST_DIR ${CMAKE_SOURCE_DIR}/tests)

set(SOURCES
    ${SRC_DIR}/PageAllocator.cpp
    ${SRC_DIR}/CentralCache.cpp
    ${SRC_DIR}/ThreadCache.cpp
)

include_directories(${INC_DIR})

add_executable(unit_test
    ${SOURCES}
    ${TEST_DIR}/UnitTest.cpp
)

add_executable(perf_test
    ${SOURCES}
    ${TEST_DIR}/PerformanceTest.cpp
)

add_executable(scenario_bench
    ${SOURCES}
    ${TEST_DIR}/ScenarioBenchmark.cpp
)

add_executable(tuning_sweep
    ${SOURCES}
    ${TEST_DIR}/TuningSweep.cpp
)

target_link_libraries(unit_test PRIVATE Threads::Threads)
target_link_libraries(perf_test PRIVATE Threads::Threads)
target_link_libraries(scenario_bench PRIVATE Threads::Threads)
target_link_libraries(tuning_sweep PRIVATE Threads::Threads)
