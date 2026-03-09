cmake_minimum_required(VERSION 3.20)
project(gc_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Warning flags
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -fno-exceptions
)

# Sources
set(GC_SOURCES
    src/mc_protocol.cpp
    src/mc_world.cpp
    src/mc_pathfinder.cpp
    src/action_queue.cpp
    src/ffi.cpp
)

# Static library
add_library(gc_native STATIC ${GC_SOURCES})

target_include_directories(gc_native
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link to Zig library (built externally)
set(ZIG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zig/zig-out/lib" CACHE PATH "Path to Zig library output")
set(ZIG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zig/include" CACHE PATH "Path to Zig headers")

target_include_directories(gc_native PRIVATE ${ZIG_INCLUDE_DIR})

# Zig library linkage — optional at configure time, required at link time
if(EXISTS "${ZIG_LIB_DIR}/libgc_zig.a")
    target_link_libraries(gc_native PRIVATE "${ZIG_LIB_DIR}/libgc_zig.a")
else()
    message(STATUS "Zig library not found at ${ZIG_LIB_DIR}/libgc_zig.a — will need to be built before linking")
    target_link_directories(gc_native PRIVATE ${ZIG_LIB_DIR})
    target_link_libraries(gc_native PRIVATE gc_zig)
endif()

# Threading
find_package(Threads REQUIRED)
target_link_libraries(gc_native PRIVATE Threads::Threads)

# Install
install(TARGETS gc_native ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include/gc_native)
