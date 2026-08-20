cmake_minimum_required(VERSION 3.16)

project(memalloc VERSION 0.2.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(memalloc INTERFACE)
target_include_directories(memalloc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(memalloc INTERFACE cxx_std_17)

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
    foreach(name test_arena test_pool test_pool_property test_synchronized test_contracts)
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
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/memallocConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/memallocConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/memalloc)
endif()
