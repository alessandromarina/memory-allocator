cmake_minimum_required(VERSION 3.16)

project(memalloc_consumer LANGUAGES CXX)

find_package(memalloc 0.2 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE memalloc::memalloc)
