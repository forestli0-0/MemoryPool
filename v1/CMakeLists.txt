# CMake minimum version
cmake_minimum_required(VERSION 3.10)

# Project name
project(MemoryPoolProject)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(include)

# Source files
file(GLOB SRC_FILES "src/*.cpp")
# CMake minimum version
cmake_minimum_required(VERSION 3.10)

# Project name
project(MemoryPoolProject)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(include)

# Source files
file(GLOB SRC_FILES "src/*.cpp")
file(GLOB TEST_FILES "tests/*.cpp")

# Add the executable for the main target
add_executable(${PROJECT_NAME} ${SRC_FILES} ${TEST_FILES})

# Add compile options
if(NOT WIN32)
    target_compile_options(${PROJECT_NAME} PRIVATE -g -pthread)
    target_link_libraries(${PROJECT_NAME} pthread)
endif()

# Link libraries (if any)
# target_link_libraries(${PROJECT_NAME} <library_name>)