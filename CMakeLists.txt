cmake_minimum_required(VERSION 3.16)

project(memalloc VERSION 0.4.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXSourceCompiles)

add_library(memalloc INTERFACE)
target_include_directories(memalloc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(memalloc INTERFACE cxx_std_17)

set(memalloc_atomic_probe [[
#include <atomic>
#include <cstdint>
int main() {
    std::atomic<std::uint64_t> head{0};
    std::uint64_t expected = head.load();
    return head.compare_exchange_weak(expected, expected + 1) ? 0 : 1;
}
]])
check_cxx_source_compiles("${memalloc_atomic_probe}" MEMALLOC_HAS_INLINE_64BIT_ATOMICS)
if(NOT MEMALLOC_HAS_INLINE_64BIT_ATOMICS)
    set(CMAKE_REQUIRED_LIBRARIES atomic)
    check_cxx_source_compiles("${memalloc_atomic_probe}" MEMALLOC_NEEDS_LIBATOMIC)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(MEMALLOC_NEEDS_LIBATOMIC)
        target_link_libraries(memalloc INTERFACE atomic)
    else()
        message(WARNING "memalloc: 64 bit atomics do not link, lock_free_pool_resource is unusable")
    endif()
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(memalloc_standalone ON)
else()
    set(memalloc_standalone OFF)
endif()

option(MEMALLOC_BUILD_TESTS "Build the memalloc tests" ${memalloc_standalone})
option(MEMALLOC_BUILD_BENCHMARKS "Build the memalloc benchmarks" ${memalloc_standalone})
option(MEMALLOC_INSTALL "Generate the memalloc install rules" ${memalloc_standalone})

if(MEMALLOC_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    foreach(name test_arena test_pool test_pool_property test_synchronized test_lock_free_pool
                 test_contracts)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE memalloc Threads::Threads)
        target_include_directories(${name} PRIVATE tests)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()

    target_compile_definitions(test_contracts PRIVATE
        MEMALLOC_DEBUG_CHECKS=1
        MEMALLOC_DEBUG_SLOW_CHECKS=1)
endif()

if(MEMALLOC_BUILD_BENCHMARKS)
    find_package(Threads REQUIRED)
    add_executable(bench_alloc bench/bench_alloc.cpp)
    target_link_libraries(bench_alloc PRIVATE memalloc Threads::Threads)
endif()

if(MEMALLOC_INSTALL)
    install(TARGETS memalloc EXPORT memallocTargets)
    install(DIRECTORY include/memalloc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT memallocTargets
        NAMESPACE memalloc::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/memalloc)

    configure_package_config_file(cmake/memallocConfig.cmake.in memallocConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/memalloc)
    write_basic_package_version_file(memallocConfigVersion.cmake
        COMPATIBILITY SameMinorVersion
        ARCH_INDEPENDENT)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/memallocConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/memallocConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/memalloc)
endif()
